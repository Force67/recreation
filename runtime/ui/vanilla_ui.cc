#include "runtime/ui/vanilla_ui.h"

#if defined(RECREATION_HAS_UGUI)

#include <base/containers/unordered_map.h>
#include <base/memory/move.h>
#include <base/option.h>
#include <stb_image.h>
#include <ugui/svg/svg.h>
#include <ugui/ultragui.h>
#include <ugui/widgets/image.h>

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
  for (mem_size i = 0; i <= manifest.size(); ++i) {
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
  return true;
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

base::Vector<base::String> VanillaScreenNames() {
  return {};
}

base::String VanillaScreenDir() {
  return {};
}

bool LoadVanillaScreen(base::StringRef, base::StringRef, VanillaScreen&) {
  return false;
}

u32 BindVanillaImages(ugui::UIContext&,
                      ugui::TextureBackend&,
                      base::StringRef,
                      const VanillaScreen&) {
  return 0;
}

}  // namespace rx::ui

#endif  // RECREATION_HAS_UGUI
