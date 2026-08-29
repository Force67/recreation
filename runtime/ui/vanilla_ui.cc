#include "runtime/ui/vanilla_ui.h"

#if defined(RECREATION_HAS_UGUI)

#include <base/containers/unordered_map.h>
#include <base/memory/move.h>
#include <base/option.h>
#include <stb_image.h>
#include <ugui/svg/svg.h>
#include <ugui/ultragui.h>
#include <ugui/widgets/image.h>
#include <ugui/widgets/text.h>
#include <ugui/widgets/widget.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/log.h"

namespace rx::ui {
namespace {

namespace fs = std::filesystem;

// Namespace scope so they register before base::InitOptionsFromEnv runs.
base::Option<const char*> VanillaUi{"ui.vanilla", nullptr, "RX_VANILLA_UI"};
base::Option<const char*> VanillaUiDir{"ui.vanilla.dir", nullptr, "RX_VANILLA_UI_DIR"};

// An asset already on the GPU, with the natural size the image widget needs.
struct UploadedImage {
  ugui::TextureId texture = ugui::kNullTextureId;
  f32 width = 0;
  f32 height = 0;
};

base::String ReadTextFile(const fs::path& path) {
  std::ifstream file(path.c_str(), std::ios::binary);
  if (!file)
    return {};
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

}  // namespace

base::UnorderedMap<base::String, base::String> LoadVanillaStrings(base::StringRef dir) {
  base::UnorderedMap<base::String, base::String> out;
  const base::String text =
      ReadTextFile(fs::path(base::String(dir).c_str()) / "strings.txt");
  base::String key;
  base::String value;
  bool on_value = false;
  for (mem_size i = 0; i <= text.size(); ++i) {
    const char c = i < text.size() ? text[i] : '\n';
    if (c == '\t' && !on_value) {
      on_value = true;
      continue;
    }
    if (c == '\n' || c == '\r') {
      if (!key.empty())
        out[key] = value;
      key = base::String();
      value = base::String();
      on_value = false;
      continue;
    }
    (on_value ? value : key).push_back(c);
  }
  return out;
}

namespace {

ugui::wid FirstTextUnder(ugui::WidgetRegistry& world, ugui::wid root) {
  const ugui::WidgetNode* node = world.Get<ugui::WidgetNode>(root);
  if (node && node->kind == ugui::WidgetKind::kText)
    return root;
  const ugui::Hierarchy* h = world.Get<ugui::Hierarchy>(root);
  if (!h)
    return ugui::kNullWidget;
  for (ugui::wid child : h->children) {
    const ugui::wid found = FirstTextUnder(world, child);
    if (found.valid())
      return found;
  }
  return ugui::kNullWidget;
}

}  // namespace

void SetVanillaText(ugui::UIContext& ui, base::StringRef widget, base::StringRef text) {
  const ugui::wid root = ui.FindWidget(base::String(widget).c_str());
  if (!root.valid())
    return;
  const ugui::wid label = FirstTextUnder(ui.world(), root);
  if (label.valid())
    ugui::SetText(label, base::String(text).c_str());
}

void SetVanillaTextColor(ugui::UIContext& ui, base::StringRef widget, u32 rgb) {
  const ugui::wid root = ui.FindWidget(base::String(widget).c_str());
  if (!root.valid())
    return;
  ugui::WidgetRegistry& world = ui.world();
  const ugui::wid label = FirstTextUnder(world, root);
  ugui::StyleC* sc = label.valid() ? world.Get<ugui::StyleC>(label) : nullptr;
  if (!sc)
    return;
  ugui::Style style = sc->style;
  style.text_color = ugui::Color::FromHex(rgb);
  ugui::SetStyle(world, label, style);
}

namespace {

void SetSubtreeOpacity(ugui::WidgetRegistry& world, ugui::wid root, f32 opacity) {
  if (ugui::StyleC* sc = world.Get<ugui::StyleC>(root)) {
    ugui::Style style = sc->style;
    style.opacity = opacity;
    ugui::SetStyle(world, root, style);
  }
  const ugui::Hierarchy* h = world.Get<ugui::Hierarchy>(root);
  if (!h)
    return;
  for (ugui::wid child : h->children)
    SetSubtreeOpacity(world, child, opacity);
}

}  // namespace

void ShowVanillaSubtree(ugui::UIContext& ui, base::StringRef widget, bool show) {
  const ugui::wid w = ui.FindWidget(base::String(widget).c_str());
  if (w.valid())
    SetSubtreeOpacity(ui.world(), w, show ? 1.0f : 0.0f);
}

base::Vector<base::String> VanillaScreenNames() {
  base::Vector<base::String> out;
  const char* value = VanillaUi.get();
  if (!value || !*value)
    return out;
  base::String current;
  for (const char* p = value;; ++p) {
    if (*p == ',' || *p == '\0') {
      if (!current.empty())
        out.push_back(base::move(current));
      current = base::String();
      if (*p == '\0')
        break;
      continue;
    }
    if (*p != ' ')
      current.push_back(*p);
  }
  return out;
}

base::String VanillaScreenDir() {
  if (const char* env = VanillaUiDir.get(); env && *env)
    return env;
#ifdef RECREATION_VANILLA_UI_DIR_DEFAULT
  return RECREATION_VANILLA_UI_DIR_DEFAULT;
#else
  return "assets/ui/vanilla";
#endif
}

bool LoadVanillaScreen(base::StringRef dir, base::StringRef name, VanillaScreen& out) {
  const fs::path root(base::String(dir).c_str());
  const base::String stem(name);
  const fs::path markup = root / (stem + ".ugui").c_str();

  out.markup = ReadTextFile(markup);
  if (out.markup.empty()) {
    RX_WARN("vanilla ui: {} not found (run tools/swfdump --ugui to translate it)",
            markup.string());
    return false;
  }
  out.name = stem;

  // The manifest is one "widget<TAB>file" line per image; a screen made only of
  // panels and text legitimately has none.
  const base::String manifest = ReadTextFile(root / (stem + ".manifest").c_str());
  base::String widget;
  base::String file;
  bool on_file = false;
  // Leading "!stage<tab>w<tab>h": the size the movie was authored against.
  mem_size start = 0;
  if (manifest.size() > 7 && manifest[0] == '!') {
    mem_size end = 0;
    while (end < manifest.size() && manifest[end] != '\n')
      ++end;
    base::String field;
    int index = 0;
    for (mem_size i = 6; i <= end; ++i) {
      const char c = i < end ? manifest[i] : '\t';
      if (c == '\t' || i == end) {
        if (!field.empty()) {
          f32 value = 0;
          f32 fraction = 0;
          f32 place = 0.1f;
          bool after_point = false;
          for (mem_size k = 0; k < field.size(); ++k) {
            const char d = field[k];
            if (d == '.') {
              after_point = true;
            } else if (d >= '0' && d <= '9') {
              if (after_point) {
                fraction += static_cast<f32>(d - '0') * place;
                place *= 0.1f;
              } else {
                value = value * 10 + static_cast<f32>(d - '0');
              }
            }
          }
          const f32 number = value + fraction;
          if (index == 0)
            out.stage_width = number;
          else if (index == 1)
            out.stage_height = number;
          ++index;
          field = base::String();
        }
        continue;
      }
      field.push_back(c);
    }
    start = end + 1;
  }
  for (mem_size i = start; i <= manifest.size(); ++i) {
    const char c = i < manifest.size() ? manifest[i] : '\n';
    if (c == '\t' && !on_file) {
      on_file = true;
      continue;
    }
    if (c == '\n' || c == '\r') {
      if (!widget.empty() && !file.empty())
        out.images.push_back(base::MakePair(base::move(widget), base::move(file)));
      widget = base::String();
      file = base::String();
      on_file = false;
      continue;
    }
    if (on_file)
      file.push_back(c);
    else
      widget.push_back(c);
  }
  // Which typefaces the markup asks for: "font: Futura Condensed Medium;".
  for (mem_size i = 0; i + 6 < out.markup.size(); ++i) {
    if (out.markup[i] != 'f' || out.markup[i + 1] != 'o' || out.markup[i + 2] != 'n' ||
        out.markup[i + 3] != 't' || out.markup[i + 4] != ':' || out.markup[i + 5] != ' ')
      continue;
    base::String family;
    mem_size k = i + 6;
    for (; k < out.markup.size() && out.markup[k] != ';'; ++k)
      family.push_back(out.markup[k]);
    i = k;
    bool known = false;
    for (const base::String& seen : out.fonts)
      known = known || seen == family;
    if (!known && !family.empty())
      out.fonts.push_back(base::move(family));
  }
  return true;
}

u32 LoadVanillaFonts(ugui::UIContext& ui,
                     base::StringRef dir,
                     const base::Vector<VanillaScreen>& screens) {
  const fs::path root = fs::path(base::String(dir).c_str()) / "fonts";
  std::error_code ec;
  if (!fs::is_directory(root, ec))
    return 0;

  base::Vector<base::String> wanted;
  for (const VanillaScreen& screen : screens) {
    for (const base::String& family : screen.fonts) {
      bool known = false;
      for (const base::String& seen : wanted)
        known = known || seen == family;
      if (!known)
        wanted.push_back(family);
    }
  }

  u32 loaded = 0;
  for (const base::String& family : wanted) {
    const fs::path path = root / (base::String(family) + ".ttf").c_str();
    if (!fs::is_regular_file(path, ec)) {
      RX_WARN("vanilla ui: {} not converted, falling back to the default font",
              family);
      continue;
    }
    const ugui::FontHandle handle = ui.LoadFont(path.string().c_str());
    if (handle == ugui::kInvalidFont) {
      RX_WARN("vanilla ui: cannot load {}", path.string());
      continue;
    }
    ui.builder().RegisterFont(family.c_str(), handle);
    ++loaded;
  }
  return loaded;
}

namespace {
// Everything uploaded for the currently bound screens, so it can be handed back
// when they are bound again.
base::Vector<ugui::TextureId>& BoundTextures() {
  static base::Vector<ugui::TextureId> textures;
  return textures;
}
}  // namespace

void ReleaseVanillaImages(ugui::TextureBackend& backend) {
  base::Vector<ugui::TextureId>& textures = BoundTextures();
  for (ugui::TextureId texture : textures)
    backend.DestroyTexture(texture);
  textures.clear();
}

u32 BindVanillaImages(ugui::UIContext& ui,
                      ugui::TextureBackend& backend,
                      base::StringRef dir,
                      const VanillaScreen& screen) {
  const fs::path root(base::String(dir).c_str());
  // One texture per asset, not per widget: a movie reuses the same art all over
  // its display list (a single invisible backing plate can appear 175 times).
  base::UnorderedMap<base::String, UploadedImage> uploaded;
  u32 bound = 0;
  for (const auto& entry : screen.images) {
    const ugui::wid widget = ui.FindWidget(entry.first.c_str());
    if (!widget.valid())
      continue;

    const base::String file = entry.second;
    if (const UploadedImage* cached = uploaded.find(file)) {
      ugui::SetImageTexture(widget, cached->texture, cached->width, cached->height);
      ++bound;
      continue;
    }
    const fs::path path = root / file.c_str();
    const bool is_svg = file.size() > 4 && file[file.size() - 4] == '.' &&
                        file[file.size() - 3] == 's' && file[file.size() - 2] == 'v' &&
                        file[file.size() - 1] == 'g';

    if (is_svg) {
      // Rasterized at the document's own size: the exporter writes the viewBox
      // in the same pixel units the markup positions the widget in, so the
      // texture comes out at the size the layout asks for.
      ugui::SvgImage image;
      if (!ugui::LoadSvg(path.string().c_str(), image) || image.pixels.empty()) {
        RX_WARN("vanilla ui: cannot rasterize {}", path.string());
        continue;
      }
      const ugui::TextureId texture =
          backend.CreateTexture(image.width, image.height, ugui::RHIFormat::kRgba8Unorm,
                                image.pixels.data(), ugui::RHIFilter::kLinear);
      if (texture == ugui::kNullTextureId)
        continue;
      BoundTextures().push_back(texture);
      uploaded[file] = UploadedImage{texture, static_cast<f32>(image.width),
                                     static_cast<f32>(image.height)};
      ugui::SetImageTexture(widget, texture, static_cast<float>(image.width),
                            static_cast<float>(image.height));
      ++bound;
      continue;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load(path.string().c_str(), &width, &height, &channels, 4);
    if (!pixels) {
      RX_WARN("vanilla ui: cannot decode {}", path.string());
      continue;
    }
    const ugui::TextureId texture =
        backend.CreateTexture(static_cast<u32>(width), static_cast<u32>(height),
                              ugui::RHIFormat::kRgba8Unorm, pixels,
                              ugui::RHIFilter::kLinear);
    stbi_image_free(pixels);
    if (texture == ugui::kNullTextureId)
      continue;
    BoundTextures().push_back(texture);
    uploaded[file] = UploadedImage{texture, static_cast<f32>(width),
                                   static_cast<f32>(height)};
    ugui::SetImageTexture(widget, texture, static_cast<float>(width),
                          static_cast<float>(height));
    ++bound;
  }
  return bound;
}

}  // namespace rx::ui

#else  // RECREATION_HAS_UGUI

namespace rx::ui {

void SetVanillaText(ugui::UIContext&, base::StringRef, base::StringRef) {}
void SetVanillaTextColor(ugui::UIContext&, base::StringRef, u32) {}
void ShowVanillaSubtree(ugui::UIContext&, base::StringRef, bool) {}
void ReleaseVanillaImages(ugui::TextureBackend&) {}

base::Vector<base::String> VanillaScreenNames() {
  return {};
}

base::String VanillaScreenDir() {
  return {};
}

base::UnorderedMap<base::String, base::String> LoadVanillaStrings(base::StringRef) {
  return {};
}

bool LoadVanillaScreen(base::StringRef, base::StringRef, VanillaScreen&) {
  return false;
}

u32 LoadVanillaFonts(ugui::UIContext&, base::StringRef, const base::Vector<VanillaScreen>&) {
  return 0;
}

u32 BindVanillaImages(ugui::UIContext&,
                      ugui::TextureBackend&,
                      base::StringRef,
                      const VanillaScreen&) {
  return 0;
}

}  // namespace rx::ui

#endif  // RECREATION_HAS_UGUI
