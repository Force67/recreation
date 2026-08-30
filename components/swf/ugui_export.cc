#include "components/swf/ugui_export.h"

#include <base/containers/unordered_map.h>
#include <base/containers/unordered_set.h>
#include <base/memory/move.h>
#include <base/strings/format.h>

#include <cmath>

#include "components/swf/abc.h"
#include "components/swf/avm1.h"
#include "components/swf/decompile.h"
#include "components/swf/svg_export.h"

namespace rx::swf {
namespace {

base::String Sanitize(base::StringRef text, base::StringRef fallback) {
  base::String out;
  for (mem_size i = 0; i < text.size(); ++i) {
    const char c = text[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_';
    out.push_back(ok ? c : '_');
  }
  mem_size start = 0;
  while (start < out.size() && out[start] == '_')
    ++start;
  base::String trimmed;
  if (start != 0 && out[0] >= '0' && out[0] <= '9')
    trimmed.push_back('n');
  for (mem_size i = start; i < out.size(); ++i)
    trimmed.push_back(out[i]);
  if (trimmed.empty())
    return base::String(fallback);
  if (trimmed[0] >= '0' && trimmed[0] <= '9')
    trimmed = base::Format("n{}", trimmed);
  return trimmed;
}

// An import entry is "url#symbol"; these split it without allocating.
base::StringRef UrlOf(base::StringRef entry) {
  for (mem_size i = 0; i < entry.size(); ++i)
    if (entry[i] == '#')
      return entry.substr(0, i);
  return entry;
}

base::StringRef SymbolOf(base::StringRef entry) {
  for (mem_size i = 0; i < entry.size(); ++i)
    if (entry[i] == '#')
      return entry.substr(i + 1);
  return entry;
}

// Path comparison for import urls: case and separator insensitive, and a match
// on the trailing file name is enough because a menu names its components
// relative to the interface directory.
bool SamePath(base::StringRef a, base::StringRef b) {
  auto normalize = [](base::StringRef path) {
    base::String out;
    for (mem_size i = 0; i < path.size(); ++i) {
      char c = path[i];
      if (c == '\\')
        c = '/';
      if (c >= 'A' && c <= 'Z')
        c = static_cast<char>(c - 'A' + 'a');
      out.push_back(c);
    }
    return out;
  };
  const base::String left = normalize(a);
  const base::String right = normalize(b);
  if (left.empty() || right.empty())
    return false;
  // An import names a movie by a path that rarely matches the archive's, so the
  // comparison is on the file name. A substring test would let "list.gfx" claim
  // "interface/itemlist.gfx", and with every movie in the archive kept loaded
  // the first wrong match wins silently.
  auto file_name = [](const base::String& path) -> base::StringRef {
    const mem_size slash = path.rfind('/');
    return slash == base::String::npos
               ? base::StringRef(path)
               : base::StringRef(path).subslice(slash + 1, path.size() - slash - 1);
  };
  return file_name(left) == file_name(right);
}

// Bethesda leaves a developer overlay inside several menus and hides it from
// ActionScript; the instance always says so in its own name.
bool IsDebugInstance(base::StringRef name) {
  for (mem_size i = 0; i + 5 <= name.size(); ++i) {
    const bool match = (name[i] == 'D' || name[i] == 'd') &&
                       (name[i + 1] == 'e' || name[i + 1] == 'E') &&
                       (name[i + 2] == 'b' || name[i + 2] == 'B') &&
                       (name[i + 3] == 'u' || name[i + 3] == 'U') &&
                       (name[i + 4] == 'g' || name[i + 4] == 'G');
    if (match)
      return true;
  }
  return false;
}

base::String Color(Rgba c) {
  if (c.a == 255)
    return base::Format("#{:02x}{:02x}{:02x}", static_cast<u32>(c.r),
                        static_cast<u32>(c.g), static_cast<u32>(c.b));
  return base::Format("#{:02x}{:02x}{:02x}{:02x}", static_cast<u32>(c.r),
                      static_cast<u32>(c.g), static_cast<u32>(c.b),
                      static_cast<u32>(c.a));
}

base::String Number(f32 v) {
  if (v == static_cast<f32>(static_cast<i32>(v)))
    return base::Format("{}", static_cast<i32>(v));
  return base::Format("{:.2f}", v);
}

// A length always carries its unit. ugui reads a bare decimal between 0 and 1
// as a flex fraction rather than pixels, and a movie's coordinates land in that
// range often enough (a half-pixel nudge on a list row) that leaving the unit
// off silently collapses those widgets.
base::String Px(f32 v) {
  base::String out = Number(v);
  out += "px";
  return out;
}

// The screen-space box a character occupies once `m` is applied to its local
// bounds, decomposed so a rotated placement keeps its real size instead of
// growing to the axis-aligned hull.
struct Box {
  f32 left = 0, top = 0, width = 0, height = 0, rotation = 0;
};

Box PlaceBox(const Matrix& m, const Rect& local) {
  const f32 sx = std::sqrt(m.scale_x * m.scale_x + m.rotate_skew0 * m.rotate_skew0);
  const f32 sy = std::sqrt(m.rotate_skew1 * m.rotate_skew1 + m.scale_y * m.scale_y);
  const f32 cx_local = static_cast<f32>(local.x_min + local.x_max) * 0.5f;
  const f32 cy_local = static_cast<f32>(local.y_min + local.y_max) * 0.5f;
  const f32 cx = m.scale_x * cx_local + m.rotate_skew1 * cy_local +
                 static_cast<f32>(m.translate_x);
  const f32 cy = m.rotate_skew0 * cx_local + m.scale_y * cy_local +
                 static_cast<f32>(m.translate_y);

  Box box;
  box.width = ToPixels(local.width()) * sx;
  box.height = ToPixels(local.height()) * sy;
  box.left = ToPixels(static_cast<i32>(cx)) - box.width * 0.5f;
  box.top = ToPixels(static_cast<i32>(cy)) - box.height * 0.5f;
  box.rotation = m.RotationDegrees();
  return box;
}

// Applies frames 0..`frame` of a timeline to get the display list as it stands
// when that frame is showing, sorted so lower depths come first.
base::Vector<Place> DisplayList(const Timeline& timeline, u32 frame) {
  base::Vector<Place> list;
  const mem_size last = timeline.frames.size() == 0
                            ? 0
                            : (frame + 1 < timeline.frames.size() ? frame + 1
                                                                  : timeline.frames.size());
  for (mem_size f = 0; f < last; ++f) {
    const Frame& source = timeline.frames[f];
    for (u16 depth : source.removes) {
      for (mem_size i = 0; i < list.size(); ++i) {
        if (list[i].depth == depth) {
          list.erase(i);
          break;
        }
      }
    }
    for (const Place& place : source.places) {
      mem_size existing = list.size();
      for (mem_size i = 0; i < list.size(); ++i) {
        if (list[i].depth == place.depth) {
          existing = i;
          break;
        }
      }
      if (existing == list.size()) {
        list.push_back(place);
        continue;
      }
      if (!place.move && place.has_character) {
        list[existing] = place;
        continue;
      }
      // A move updates only the fields it carries.
      Place& target = list[existing];
      if (place.has_character)
        target.character_id = place.character_id;
      if (place.has_matrix) {
        target.matrix = place.matrix;
        target.has_matrix = true;
      }
      if (place.has_color_transform) {
        target.color_transform = place.color_transform;
        target.has_color_transform = true;
      }
      if (!place.name.empty())
        target.name = place.name;
      if (place.clip_depth != 0)
        target.clip_depth = place.clip_depth;
    }
  }
  for (mem_size i = 1; i < list.size(); ++i) {
    for (mem_size j = i; j > 0 && list[j].depth < list[j - 1].depth; --j) {
      Place tmp = base::move(list[j]);
      list[j] = base::move(list[j - 1]);
      list[j - 1] = base::move(tmp);
    }
  }
  return list;
}

class Exporter {
 public:
  Exporter(const Movie& movie, const UguiExportOptions& options)
      : movie_(movie), current_(&movie), options_(options) {
    LoadListBindings();
  }

  UguiScreen Run();

 private:
  void EmitTimeline(const Timeline& timeline,
                    const Matrix& parent_matrix,
                    const ColorTransform& parent_color,
                    bool parent_revealed,
                    const Box& parent_box,
                    u32 indent,
                    u32 depth);
  void EmitPlace(const Place& place,
                 const Matrix& absolute,
                 const ColorTransform& color,
                 bool revealed,
                 const Box& parent_box,
                 u32 indent,
                 u32 depth);
  void EmitShape(const Shape& shape,
                 const ColorTransform& color,
                 bool revealed,
                 const Place& place,
                 const Box& box,
                 u32 indent);
  void EmitEditText(const EditText& text,
                    const ColorTransform& color,
                    const Place& place,
                    const Box& box,
                    u32 indent);
  void EmitStaticText(const StaticText& text,
                      const ColorTransform& color,
                      const Box& box,
                      u32 indent);

  Rect SpriteBounds(const Timeline& timeline, u32 depth);
  Rect CharacterBounds(u16 character_id, u32 depth);
  // An imported placeholder: finds the movie and character that own the symbol
  // this id stands in for. Returns null when the import was not supplied.
  const Movie* ResolveImport(u16 character_id, u32& index, u16& resolved) const;

  // Resolves a "$KEY" through the interface string table, if one was supplied.
  base::String Localize(base::StringRef text) const;
  // The family a text field's font resolves to, empty when it cannot be named.
  base::String FontFamily(const EditText& text, base::StringRef html_face) const;
  base::String UniqueName(base::StringRef preferred, base::StringRef fallback);
  base::String NameFor(const Place& place, base::StringRef fallback);
  base::String ShapeAsset(const Shape& shape);
  base::String BitmapAsset(const Bitmap& bitmap);
  void Bind(base::StringRef widget, base::StringRef file);

  void Line(u32 indent, base::StringRef text);

  // The style fragment every absolutely placed widget shares.
  // `color` is the transform accumulated down the display list; pass null for a
  // container, whose own opacity would do nothing (ugui does not inherit it,
  // which is exactly why the transform is concatenated here instead).
  base::String Placement(const Box& box, const ColorTransform* color) const;
  // The same, for a widget that only holds others. Children are positioned from
  // their own absolute matrices, so a rotation here would turn them twice.
  base::String ContainerPlacement(const Box& box) const;

  const Movie& movie_;
  // The movie whose dictionary the walk is reading. Normally `movie_`; an
  // imported placeholder switches it to the movie that owns the symbol.
  const Movie* current_ = nullptr;
  u32 current_index_ = 0;  // salts asset keys, which are only unique per movie
  // AS3 lists ship empty and are filled by code; the bytecode says with what.
  base::Vector<ListBinding> list_bindings_;
  base::String current_owner_;  // export symbol of the sprite being walked
  void LoadListBindings();
  const ListBinding* FindListBinding(base::StringRef owner, base::StringRef instance) const;
  void StampListRows(base::StringRef owner,
                     const Place& place,
                     const Matrix& absolute,
                     const ColorTransform& color,
                     bool revealed,
                     const Box& box,
                     u32 indent,
                     u32 depth);
  const UguiExportOptions& options_;
  UguiScreen out_;
  base::UnorderedMap<base::String, u32> used_names_;
  base::UnorderedMap<u32, base::String> shape_files_;
  base::UnorderedMap<u32, base::String> bitmap_files_;
  base::UnorderedMap<u32, Rect> sprite_bounds_;
  base::UnorderedSet<u32> bounds_in_progress_;
};

void Exporter::Line(u32 indent, base::StringRef text) {
  for (u32 i = 0; i < indent; ++i)
    out_.markup += "  ";
  out_.markup += text;
  out_.markup += '\n';
}

base::String Exporter::Localize(base::StringRef text) const {
  if (!options_.strings || text.empty() || text[0] != '$')
    return base::String(text);
  if (const base::String* hit = options_.strings->find(base::String(text)))
    return *hit;
  return base::String(text);
}

base::String Exporter::FontFamily(const EditText& text, base::StringRef html_face) const {
  if (!options_.font_families)
    return base::String();
  // The HTML face wins: a field styled that way is what Scaleform actually
  // renders, whatever the DefineEditText tag says.
  base::StringRef symbol = html_face;
  if (symbol.empty())
    symbol = text.font_class;
  if (symbol.empty()) {
    if (const base::String* imported = current_->imported_symbols.find(text.font_id))
      symbol = SymbolOf(*imported);
    else if (const Font* font = current_->FindFont(text.font_id))
      symbol = base::StringRef(font->name);
  }
  if (symbol.empty())
    return base::String();
  if (const base::String* family = options_.font_families->find(base::String(symbol)))
    return *family;
  return base::String();
}

base::String Exporter::UniqueName(base::StringRef preferred, base::StringRef fallback) {
  base::String name = Sanitize(preferred, fallback);
  u32* seen = used_names_.find(name);
  if (!seen) {
    used_names_[name] = 1;
    return name;
  }
  u32 next = ++(*seen);
  base::String candidate = base::Format("{}_{}", name, next);
  while (used_names_.contains(candidate)) {
    ++next;
    candidate = base::Format("{}_{}", name, next);
  }
  used_names_[candidate] = 1;
  return candidate;
}

// The instance name is what ActionScript addresses the object by, so it makes
// the best widget name; the linkage export name is the next best.
base::String Exporter::NameFor(const Place& place, base::StringRef fallback) {
  if (!place.name.empty())
    return UniqueName(place.name, fallback);
  const base::StringRef exported = current_->ExportName(place.character_id);
  if (!exported.empty())
    return UniqueName(exported, fallback);
  return UniqueName(fallback, fallback);
}

void Exporter::Bind(base::StringRef widget, base::StringRef file) {
  out_.manifest += widget;
  out_.manifest += '\t';
  out_.manifest += file;
  out_.manifest += '\n';
}

base::String Exporter::ShapeAsset(const Shape& shape) {
  const u32 key = current_index_ << 16 | shape.id;
  const base::String* existing = shape_files_.find(key);
  if (existing)
    return *existing;
  base::String file = current_index_ == 0
                          ? base::Format("{}/shape_{}.svg", options_.name, shape.id)
                          : base::Format("{}/i{}_shape_{}.svg", options_.name,
                                         current_index_, shape.id);
  base::String svg = ShapeToSvg(shape);
  ExportedAsset asset;
  asset.file = file;
  for (mem_size i = 0; i < svg.size(); ++i)
    asset.bytes.push_back(static_cast<u8>(svg[i]));
  out_.assets.push_back(base::move(asset));
  shape_files_[key] = file;
  return file;
}

base::String Exporter::BitmapAsset(const Bitmap& bitmap) {
  const u32 key = current_index_ << 16 | bitmap.id;
  const base::String* existing = bitmap_files_.find(key);
  if (existing)
    return *existing;
  base::String prefix = current_index_ == 0
                            ? base::Format("{}/image", options_.name)
                            : base::Format("{}/i{}_image", options_.name, current_index_);
  base::String file;
  ExportedAsset asset;
  if (bitmap.is_jpeg()) {
    file = base::Format("{}_{}.jpg", prefix, bitmap.id);
    asset.bytes = bitmap.jpeg;
  } else {
    file = base::Format("{}_{}.png", prefix, bitmap.id);
    asset.bytes = EncodePng(bitmap.width, bitmap.height,
                            ByteSpan{bitmap.rgba.data(), bitmap.rgba.size()});
  }
  if (asset.bytes.empty())
    return base::String();
  asset.file = file;
  out_.assets.push_back(base::move(asset));
  bitmap_files_[key] = file;
  return file;
}

const Movie* Exporter::ResolveImport(u16 character_id, u32& index, u16& resolved) const {
  if (!options_.imports)
    return nullptr;
  const base::String* entry = current_->imported_symbols.find(character_id);
  if (!entry)
    return nullptr;
  const base::StringRef url = UrlOf(*entry);
  const base::StringRef symbol = SymbolOf(*entry);
  for (mem_size i = 0; i < options_.imports->size(); ++i) {
    const ImportedMovie& candidate = (*options_.imports)[i];
    if (!candidate.movie || !SamePath(candidate.path, url))
      continue;
    for (const auto& exported : candidate.movie->exports) {
      if (exported.value != symbol)
        continue;
      index = static_cast<u32>(i + 1);
      resolved = exported.key;
      return candidate.movie;
    }
  }
  return nullptr;
}

Rect Exporter::CharacterBounds(u16 character_id, u32 depth) {
  // Buttons place characters and imports resolve into other movies, and either
  // can name its way back round to where it started. Only the sprite path below
  // keeps its own cycle set, so the depth cap has to cover the rest.
  if (depth > options_.max_depth)
    return Rect{};
  if (const Shape* shape = current_->FindShape(character_id))
    return shape->bounds;
  if (const EditText* text = current_->FindEditText(character_id))
    return text->bounds;
  if (const StaticText* text = current_->FindStaticText(character_id))
    return text->bounds;
  if (const Bitmap* bitmap = current_->FindBitmap(character_id)) {
    Rect r;
    r.x_max = static_cast<i32>(bitmap->width * kTwipsPerPixel);
    r.y_max = static_cast<i32>(bitmap->height * kTwipsPerPixel);
    return r;
  }
  if (const ExternalImage* image = current_->FindExternalImage(character_id)) {
    Rect r;
    r.x_max = static_cast<i32>(image->width * kTwipsPerPixel);
    r.y_max = static_cast<i32>(image->height * kTwipsPerPixel);
    return r;
  }
  if (const Button* button = current_->FindButton(character_id)) {
    Rect out;
    bool first = true;
    for (const ButtonRecord& record : button->records) {
      if (!record.up)
        continue;
      const Rect child = Transform(record.matrix, CharacterBounds(record.character_id,
                                                                 depth + 1));
      if (first) {
        out = child;
        first = false;
      } else {
        out.x_min = out.x_min < child.x_min ? out.x_min : child.x_min;
        out.y_min = out.y_min < child.y_min ? out.y_min : child.y_min;
        out.x_max = out.x_max > child.x_max ? out.x_max : child.x_max;
        out.y_max = out.y_max > child.y_max ? out.y_max : child.y_max;
      }
    }
    return out;
  }
  if (const Timeline* sprite = current_->FindSprite(character_id))
    return SpriteBounds(*sprite, depth);

  u32 index = 0;
  u16 resolved = 0;
  if (const Movie* owner = ResolveImport(character_id, index, resolved)) {
    const Movie* previous = current_;
    const u32 previous_index = current_index_;
    current_ = owner;
    current_index_ = index;
    const Rect bounds = CharacterBounds(resolved, depth + 1);
    current_ = previous;
    current_index_ = previous_index;
    return bounds;
  }
  return Rect{};
}

// A sprite has no bounds of its own: it is the union of what its display list
// places, which is what a ugui panel has to be sized to.
Rect Exporter::SpriteBounds(const Timeline& timeline, u32 depth) {
  const u32 key = current_index_ << 16 | timeline.id;
  const Rect* cached = sprite_bounds_.find(key);
  if (cached)
    return *cached;
  if (depth > options_.max_depth || bounds_in_progress_.contains(key))
    return Rect{};
  bounds_in_progress_.insert(key);

  Rect out;
  bool first = true;
  for (const Place& place : DisplayList(timeline, options_.frame)) {
    if (!place.has_character || place.clip_depth != 0)
      continue;
    const Rect local = CharacterBounds(place.character_id, depth + 1);
    if (local.empty())
      continue;
    const Rect child = Transform(place.matrix, local);
    if (first) {
      out = child;
      first = false;
    } else {
      out.x_min = out.x_min < child.x_min ? out.x_min : child.x_min;
      out.y_min = out.y_min < child.y_min ? out.y_min : child.y_min;
      out.x_max = out.x_max > child.x_max ? out.x_max : child.x_max;
      out.y_max = out.y_max > child.y_max ? out.y_max : child.y_max;
    }
  }
  bounds_in_progress_.erase(key);
  sprite_bounds_[key] = out;
  return out;
}

base::String Exporter::Placement(const Box& box, const ColorTransform* color) const {
  const f32 s = options_.scale;
  base::String style = base::Format(
      "position: absolute; left: {}; top: {}; width: {}; height: {};",
      Px(box.left * s), Px(box.top * s), Px(box.width * s), Px(box.height * s));
  if (box.rotation > 0.5f || box.rotation < -0.5f)
    style += base::Format(" rotation: {};", Number(box.rotation));
  if (color) {
    const f32 alpha = color->mul_a;
    if (alpha < 0.999f)
      style += base::Format(" opacity: {:.3f};", alpha < 0 ? 0.0f : alpha);
  }
  return style;
}

base::String Exporter::ContainerPlacement(const Box& box) const {
  Box upright = box;
  upright.rotation = 0;
  return Placement(upright, nullptr);
}

void Exporter::EmitShape(const Shape& shape,
                         const ColorTransform& color,
                         bool revealed,
                         const Place& place,
                         const Box& box,
                         u32 indent) {
  const ColorTransform& cx = color;
  const bool tinted = !cx.IsIdentity();

  // Flash marks a shape that is never meant to be seen - a button hit area, a
  // component's state swatch, a bounds box - by outlining it with a stroke at
  // an alpha of a couple of 255ths. The movie hides them at runtime through
  // their own clip's alpha; drawing them here puts a coloured slab over the
  // screen they belong to.
  if (IsHitArea(shape)) {
    ++out_.skipped_count;
    return;
  }
  // Nothing to draw at all.
  if (shape.fill_paths.empty() && shape.stroke_paths.empty()) {
    ++out_.skipped_count;
    return;
  }

  Rgba solid;
  if (AsSolidRect(shape, solid)) {
    // A plain rectangle only becomes visible because the reveal forced its
    // clip's alpha up; it is backing, not art.
    if (revealed) {
      ++out_.skipped_count;
      return;
    }
    if (tinted)
      solid = Apply(cx, solid);
    const base::String name = NameFor(place, base::Format("shape{}", shape.id));
    Line(indent, base::Format("panel {} {{ {} background: {}; }}", name,
                              Placement(box, nullptr), Color(solid)));
    ++out_.widget_count;
    return;
  }

  Rgba start;
  Rgba end;
  f32 angle = 0;
  if (AsLinearGradientRect(shape, start, end, angle)) {
    if (tinted) {
      start = Apply(cx, start);
      end = Apply(cx, end);
    }
    const base::String name = NameFor(place, base::Format("shape{}", shape.id));
    Line(indent, base::Format(
                     "panel {} {{ {} background: {}; background-end: {}; "
                     "gradient-angle: {}; }}",
                     name, Placement(box, nullptr), Color(start), Color(end),
                     Number(angle)));
    ++out_.widget_count;
    return;
  }

  u16 bitmap_id = 0;
  if (AsBitmapRect(shape, bitmap_id)) {
    if (const Bitmap* bitmap = current_->FindBitmap(bitmap_id)) {
      const base::String file = BitmapAsset(*bitmap);
      if (!file.empty()) {
        const base::String name = NameFor(place, base::Format("image{}", bitmap_id));
        Line(indent, base::Format("image {} {{ {} }}", name, Placement(box, &color)));
        Bind(name, file);
        ++out_.widget_count;
        return;
      }
    }
  }

  if (!ShapeHasVectorContent(shape)) {
    ++out_.skipped_count;
    return;
  }

  const base::String file = ShapeAsset(shape);
  const base::String name = NameFor(place, base::Format("art{}", shape.id));
  Line(indent, base::Format("image {} {{ {} }}", name, Placement(box, &color)));
  Bind(name, file);
  ++out_.widget_count;
}

void Exporter::EmitEditText(const EditText& text,
                            const ColorTransform& color_transform,
                            const Place& place,
                            const Box& box,
                            u32 indent) {
  const base::String name = NameFor(place, base::Format("text{}", text.id));
  HtmlFormat format;
  const bool styled = text.html && ParseHtmlFormat(text.initial_text, format);
  const base::String authored =
      text.html ? StripHtml(text.initial_text) : text.initial_text;
  base::String content = Localize(authored);
  // Newlines would break the single-line markup value.
  base::String flat;
  for (mem_size i = 0; i < content.size(); ++i) {
    const char c = content[i];
    if (c == '\n' || c == '\r')
      flat += ' ';
    else if (c == '"')
      flat += '\'';
    else
      flat += c;
  }

  const Rgba color =
      Apply(color_transform, (styled && format.has_color) ? format.color : text.color);

  base::String style = Placement(box, nullptr);
  style += base::Format(" text: \"{}\";", flat);
  const f32 size = (styled && format.size > 0 ? format.size
                                              : ToPixels(text.font_height)) *
                   options_.scale;
  if (size > 0)
    style += base::Format(" font-size: {};", Number(size));
  const base::String family =
      FontFamily(text, styled ? base::StringRef(format.face) : base::StringRef());
  if (!family.empty())
    style += base::Format(" font: {};", family);
  style += base::Format(" color: {};", Color(color));
  if (styled && format.letter_spacing != 0)
    style += base::Format(" letter-spacing: {};",
                          Number(format.letter_spacing * options_.scale));
  const TextAlign align = (styled && format.has_align) ? format.align : text.align;
  switch (align) {
    case TextAlign::kRight:
      style += " text-align: right;";
      break;
    case TextAlign::kCenter:
      style += " text-align: center;";
      break;
    default:
      break;
  }

  base::String note;
  if (!text.variable.empty())
    note = base::Format("  // bound to {}", text.variable);
  else if (family.empty() && !text.font_class.empty())
    note = base::Format("  // font {}", text.font_class);

  Line(indent, base::Format("text {} {{ {} }}{}", name, style, note));
  ++out_.widget_count;
}

void Exporter::EmitStaticText(const StaticText& text,
                              const ColorTransform& color_transform,
                              const Box& box,
                              u32 indent) {
  base::String content;
  u16 height = 0;
  Rgba color{0, 0, 0, 255};
  for (const TextRun& run : text.runs) {
    if (const Font* font = current_->FindFont(run.font_id))
      content += ResolveRunText(*font, run);
    if (height == 0 && run.height != 0) {
      height = run.height;
      color = run.color;
    }
  }
  if (content.empty()) {
    ++out_.skipped_count;
    return;
  }
  content = Localize(content);
  base::String flat;
  for (mem_size i = 0; i < content.size(); ++i)
    flat += content[i] == '"' ? '\'' : content[i];

  const base::String name = UniqueName(base::Format("label{}", text.id),
                                       base::Format("label{}", text.id));
  base::String style = Placement(box, nullptr);
  style += base::Format(" text: \"{}\";", flat);
  if (height != 0)
    style += base::Format(" font-size: {};", Number(ToPixels(height) * options_.scale));
  style += base::Format(" color: {};", Color(Apply(color_transform, color)));
  Line(indent, base::Format("text {} {{ {} }}", name, style));
  ++out_.widget_count;
}

void Exporter::EmitPlace(const Place& place,
                         const Matrix& absolute,
                         const ColorTransform& color,
                         bool revealed,
                         const Box& parent_box,
                         u32 indent,
                         u32 depth) {
  // Buttons place characters and imports resolve into other movies, and either
  // can lead back to where it started. The sprite branch below keeps its own
  // cycle set; this covers the rest.
  if (depth > options_.max_depth) {
    ++out_.skipped_count;
    return;
  }
  if (!place.has_character || !place.visible) {
    ++out_.skipped_count;
    return;
  }
  if (IsDebugInstance(place.name)) {
    Line(indent, base::Format("// {}: the movie's own debug overlay, hidden in the "
                              "shipped game",
                              place.name));
    ++out_.skipped_count;
    return;
  }
  const Rect local = CharacterBounds(place.character_id, depth);
  // `absolute_box` is where the object lands on the stage; `box` is the same
  // rectangle expressed relative to the enclosing panel, which is what ugui's
  // absolute positioning is measured from. Children are given the absolute one.
  const Box absolute_box = PlaceBox(absolute, local);
  Box box = absolute_box;
  box.left -= parent_box.left;
  box.top -= parent_box.top;

  if (box.width < options_.min_size_px || box.height < options_.min_size_px) {
    ++out_.skipped_count;
    return;
  }

  if (const Shape* shape = current_->FindShape(place.character_id)) {
    EmitShape(*shape, color, revealed, place, box, indent);
    return;
  }
  if (const EditText* text = current_->FindEditText(place.character_id)) {
    EmitEditText(*text, color, place, box, indent);
    return;
  }
  if (const StaticText* text = current_->FindStaticText(place.character_id)) {
    EmitStaticText(*text, color, box, indent);
    return;
  }
  if (const Bitmap* bitmap = current_->FindBitmap(place.character_id)) {
    const base::String file = BitmapAsset(*bitmap);
    if (file.empty()) {
      ++out_.skipped_count;
      return;
    }
    const base::String name = NameFor(place, base::Format("image{}", bitmap->id));
    Line(indent, base::Format("image {} {{ {} }}", name, Placement(box, &color)));
    Bind(name, file);
    ++out_.widget_count;
    return;
  }

  // The .gfx export's bitmaps: the pixels are a file beside the movie, so the
  // asset is recorded by source name and the caller fetches it. Without this a
  // .gfx-only movie loses all of its raster art.
  if (const ExternalImage* image = current_->FindExternalImage(place.character_id)) {
    const base::String name = NameFor(place, base::Format("image{}", image->id));
    const base::String file =
        base::Format("{}/{}.png", options_.name, Sanitize(image->name, "image"));
    bool have = false;
    for (const ExportedAsset& asset : out_.assets)
      have = have || asset.file == file;
    if (!have) {
      ExportedAsset asset;
      asset.file = file;
      asset.widget = name;
      asset.source = image->file;
      out_.assets.push_back(base::move(asset));
    }
    Line(indent, base::Format("image {} {{ {} }}", name, Placement(box, &color)));
    Bind(name, file);
    ++out_.widget_count;
    return;
  }

  if (const Button* button = current_->FindButton(place.character_id)) {
    const base::String name = NameFor(place, base::Format("button{}", button->id));
    base::String style = ContainerPlacement(box);
    style += " cursor: pointer;";
    Line(indent, base::Format("panel {} {{ {}", name, style));
    for (mem_size i = 0; i < button->condition_flags.size(); ++i)
      Line(indent + 1, base::Format("// on({}) handler in the script listing",
                                    button->condition_flags[i]));
    for (const ButtonRecord& record : button->records) {
      if (!record.up)
        continue;
      Place synthetic;
      synthetic.depth = record.depth;
      synthetic.character_id = record.character_id;
      synthetic.has_character = true;
      synthetic.matrix = record.matrix;
      synthetic.has_matrix = true;
      synthetic.color_transform = record.color_transform;
      EmitPlace(synthetic, Concat(absolute, record.matrix),
                Concat(color, record.color_transform), revealed, absolute_box,
                indent + 1, depth + 1);
    }
    Line(indent, "}");
    ++out_.widget_count;
    return;
  }

  if (const Timeline* sprite = current_->FindSprite(place.character_id)) {
    const base::String name = NameFor(place, base::Format("sprite{}", sprite->id));
    // ugui scales a subtree uniformly; a nine-slice holds the corners while the
    // middle stretches. Note the split rather than dropping it, since a frame
    // resized the wrong way is visibly wrong.
    if (const Rect* grid = current_->scaling_grids.find(place.character_id)) {
      Line(indent, base::Format(
                       "// nine-slice in the original: left {} top {} right {} bottom {}",
                       Number(ToPixels(grid->x_min)), Number(ToPixels(grid->y_min)),
                       Number(ToPixels(grid->x_max)), Number(ToPixels(grid->y_max))));
    }
    base::String style = ContainerPlacement(box);
    if (depth >= options_.max_depth) {
      Line(indent, base::Format("panel {} {{ {} }}  // nesting limit", name, style));
      ++out_.widget_count;
      return;
    }
    base::String note;
    for (mem_size i = 0; i < place.clip_event_flags.size(); ++i) {
      const u32 flags = place.clip_event_flags[i];
      for (u32 bit = 0; bit < 19; ++bit) {
        if (!(flags & (1u << bit)))
          continue;
        const base::StringRef event = ClipEventName(bit);
        if (event.empty())
          continue;
        if (!note.empty())
          note += ", ";
        note += event;
      }
    }
    if (note.empty())
      Line(indent, base::Format("panel {} {{ {}", name, style));
    else
      Line(indent, base::Format("panel {} {{ {}  // handlers: {}", name, style, note));
    const base::String owner = current_owner_;
    base::String previous_owner = current_owner_;
    if (const base::String* symbol = current_->exports.find(sprite->id))
      current_owner_ = *symbol;
    EmitTimeline(*sprite, absolute, color, revealed, absolute_box, indent + 1,
                 depth + 1);
    // A list the code fills: stamp the rows the bytecode asks for, so the
    // translation carries the same clips an AS2 movie would have on its
    // timeline and a host can drive it the same way.
    StampListRows(owner, place, absolute, color, revealed, absolute_box, indent + 1,
                  depth + 1);
    current_owner_ = base::move(previous_owner);
    Line(indent, "}");
    ++out_.widget_count;
    return;
  }

  u32 index = 0;
  u16 resolved = 0;
  if (const Movie* owner = ResolveImport(place.character_id, index, resolved)) {
    Place spliced = place;
    spliced.character_id = resolved;
    const Movie* previous = current_;
    const u32 previous_index = current_index_;
    current_ = owner;
    current_index_ = index;
    EmitPlace(spliced, absolute, color, revealed, parent_box, indent, depth + 1);
    current_ = previous;
    current_index_ = previous_index;
    return;
  }

  ++out_.skipped_count;
}

void Exporter::LoadListBindings() {
  for (const ByteSpan& block : movie_.abc_blocks) {
    AbcFile abc;
    if (!ParseAbc(block, abc))
      continue;
    for (ListBinding& binding : ParseListBindings(abc))
      list_bindings_.push_back(base::move(binding));
  }
}

const ListBinding* Exporter::FindListBinding(base::StringRef owner,
                                             base::StringRef instance) const {
  if (owner.empty() || instance.empty())
    return nullptr;
  for (const ListBinding& binding : list_bindings_)
    if (binding.owner == owner && binding.instance == instance)
      return &binding;
  return nullptr;
}

void Exporter::StampListRows(base::StringRef owner,
                             const Place& place,
                             const Matrix& absolute,
                             const ColorTransform& color,
                             bool revealed,
                             const Box& box,
                             u32 indent,
                             u32 depth) {
  const ListBinding* binding = FindListBinding(owner, place.name);
  if (!binding || binding->count == 0)
    return;
  // The entry symbol has to be one this movie exports; the row is whatever
  // character that name is bound to.
  u16 entry_id = 0;
  for (const auto& entry : current_->exports) {
    if (entry.value == binding->entry) {
      entry_id = entry.key;
      break;
    }
  }
  if (entry_id == 0)
    return;
  const Rect bounds = CharacterBounds(entry_id, depth);
  const i32 pitch = bounds.height();
  if (pitch <= 0)
    return;

  Line(indent, base::Format("// {} rows of {}, stamped from the bytecode's own"
                            " listEntryClass / numListItems",
                            binding->count, binding->entry));
  for (u32 row = 0; row < binding->count; ++row) {
    Place stamped;
    stamped.has_character = true;
    stamped.character_id = entry_id;
    stamped.depth = static_cast<u16>(row);
    stamped.has_matrix = true;
    stamped.matrix.translate_y = static_cast<i32>(row) * pitch;
    // The instance names a host addresses the rows by, matching how the AS2
    // movies name the clips they place on the timeline.
    stamped.name = base::Format("Entry{}", row);
    EmitPlace(stamped, Concat(absolute, stamped.matrix), color, revealed, box, indent,
              depth);
  }
}

void Exporter::EmitTimeline(const Timeline& timeline,
                            const Matrix& parent_matrix,
                            const ColorTransform& parent_color,
                            bool parent_revealed,
                            const Box& parent_box,
                            u32 indent,
                            u32 depth) {
  const base::Vector<Place> list = DisplayList(timeline, options_.frame);
  mem_size i = 0;
  while (i < list.size()) {
    const Place& place = list[i];
    const Matrix absolute =
        place.has_matrix ? Concat(parent_matrix, place.matrix) : parent_matrix;
    ColorTransform color = place.has_color_transform
                               ? Concat(parent_color, place.color_transform)
                               : parent_color;
    bool revealed = parent_revealed;
    if (options_.reveal_faded && color.mul_a <= 1.0f / 255.0f) {
      color.mul_a = 1.0f;
      revealed = true;
    }

    // A clip-depth object is a Flash mask: it is never drawn, it clips every
    // depth up to clip_depth to its own shape. That is how every meter in the
    // game reveals its fill. ugui clips with overflow on a parent, so the mask
    // becomes a clipping panel and the depths it covers become its children.
    // The clip is the mask's box rather than its silhouette; every mask that
    // matters here is a rectangle, so the two agree.
    if (place.clip_depth != 0 && place.has_character) {
      const Rect local = CharacterBounds(place.character_id, depth);
      const Box mask = PlaceBox(absolute, local);
      Box relative = mask;
      relative.left -= parent_box.left;
      relative.top -= parent_box.top;

      base::String preferred = place.name;
      if (preferred.empty())
        preferred = base::Format("mask{}", place.depth);
      else
        preferred += "_mask";
      const base::String name =
          UniqueName(preferred, base::Format("mask{}", place.depth));
      Line(indent, base::Format("panel {} {{ {} overflow: hidden;  // Flash mask, "
                                "depths {}..{}",
                                name, ContainerPlacement(relative), place.depth,
                                place.clip_depth));
      ++out_.widget_count;
      ++i;
      while (i < list.size() && list[i].depth <= place.clip_depth) {
        const Matrix child = list[i].has_matrix ? Concat(parent_matrix, list[i].matrix)
                                                : parent_matrix;
        ColorTransform child_color = list[i].has_color_transform
                                         ? Concat(parent_color, list[i].color_transform)
                                         : parent_color;
        bool child_revealed = parent_revealed;
        if (options_.reveal_faded && child_color.mul_a <= 1.0f / 255.0f) {
          child_color.mul_a = 1.0f;
          child_revealed = true;
        }
        EmitPlace(list[i], child, child_color, child_revealed, mask, indent + 1, depth);
        ++i;
      }
      Line(indent, "}");
      continue;
    }

    EmitPlace(place, absolute, color, revealed, parent_box, indent, depth);
    ++i;
  }
}

UguiScreen Exporter::Run() {
  const f32 width = ToPixels(current_->frame_size.width()) * options_.scale;
  const f32 height = ToPixels(current_->frame_size.height()) * options_.scale;

  out_.markup += base::Format(
      "// Translated from the vanilla Scaleform movie by tools/swfdump.\n"
      "// Stage {}x{} at {:.1f} fps, {} characters, {} exported symbols.\n"
      "// Widget names are the ActionScript instance names, so the original\n"
      "// bindings still address the same objects.\n\n",
      Number(ToPixels(current_->frame_size.width())),
      Number(ToPixels(current_->frame_size.height())), current_->frame_rate,
      current_->characters.size(), current_->exports.size());

  if (!current_->imports.empty()) {
    out_.markup += base::Format(
        "// Imports {} symbol(s) from other movies; those subtrees are placed as\n"
        "// empty panels here because the art lives in the file named after the #:\n",
        current_->imports.size());
    for (const base::String& entry : current_->imports)
      out_.markup += base::Format("//   {}\n", entry);
    out_.markup += '\n';
  }

  // Namespaced: a movie's stem is its own name, and the host has fragments of
  // its own. Fallout 4's mainmenu.swf and recreation's main_menu.ugui both want
  // to be called "mainmenu", and two roots with one name merge into one tree.
  // Only the root is prefixed; everything inside keeps its ActionScript instance
  // name so the original bindings still address the same objects.
  const base::String root = UniqueName("vanilla_" + options_.name, "screen");
  Line(0, base::Format(
              "panel {} {{ position: absolute; left: 0; top: 0; width: {}; height: {};",
              root, Px(width), Px(height)));

  Matrix identity;
  identity.translate_x = -current_->frame_size.x_min;
  identity.translate_y = -current_->frame_size.y_min;
  Box stage;
  stage.width = ToPixels(current_->frame_size.width());
  stage.height = ToPixels(current_->frame_size.height());
  EmitTimeline(current_->root, identity, ColorTransform{}, false, stage, 1, 0);
  Line(0, "}");

  // The host has to scale the stage to its viewport the way Scaleform does, so
  // the size it was authored against leads the manifest.
  base::String manifest = base::Format("!stage\t{}\t{}\n",
                                       Number(ToPixels(movie_.frame_size.width())),
                                       Number(ToPixels(movie_.frame_size.height())));
  manifest += out_.manifest;
  out_.manifest = base::move(manifest);

  out_.script = ExportScript(movie_);
  return base::move(out_);
}

}  // namespace

UguiScreen ExportUgui(const Movie& movie, const UguiExportOptions& options) {
  Exporter exporter(movie, options);
  return exporter.Run();
}

base::String ExportScript(const Movie& movie) {
  base::String out;
  // ActionScript 3 first: when a movie has any, that is where all of its logic
  // lives and the AVM1 blocks below are at most a stub frame script.
  for (const ByteSpan& block : movie.abc_blocks) {
    AbcFile abc;
    if (!ParseAbc(block, abc) && abc.classes.empty()) {
      out += "// unreadable DoABC block\n\n";
      continue;
    }
    out += AbcDisassembly(abc);
  }
  for (const Script& script : movie.scripts) {
    out += "// ---------------------------------------------------------------\n";
    switch (script.kind) {
      case Script::Kind::kInit: {
        const base::StringRef symbol = movie.ExportName(script.sprite_id);
        out += base::Format("// #initclip for character {}{}{}\n", script.sprite_id,
                            symbol.empty() ? "" : " -> ", symbol);
        break;
      }
      case Script::Kind::kFrame:
        out += base::Format("// frame {} of {}\n", script.frame + 1,
                            script.timeline_id == 0
                                ? base::String("the root timeline")
                                : base::Format("sprite {}", script.timeline_id));
        break;
      case Script::Kind::kClipEvent:
        out += "// clip event handler\n";
        break;
      case Script::Kind::kButtonCondition:
        out += "// button condition handler\n";
        break;
    }
    out += "// ---------------------------------------------------------------\n";
    out += Decompile(script.code);
    out += '\n';
  }

  // Instance handlers ride on PlaceObject rather than on their own tag, so walk
  // the timelines for them after the frame and init scripts.
  for (const Timeline* timeline : {&movie.root}) {
    for (const Frame& frame : timeline->frames) {
      for (const Place& place : frame.places) {
        for (mem_size i = 0; i < place.clip_event_code.size(); ++i) {
          out += base::Format("// on the instance \"{}\" at depth {}\n",
                              place.name.empty() ? base::String("<unnamed>") : place.name,
                              place.depth);
          out += Decompile(place.clip_event_code[i]);
          out += '\n';
        }
      }
    }
  }
  return out;
}

}  // namespace rx::swf
