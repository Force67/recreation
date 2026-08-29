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
      : movie_(movie), options_(options) {}

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
  const UguiExportOptions& options_;
  UguiScreen out_;
  base::UnorderedMap<base::String, u32> used_names_;
  base::UnorderedMap<u16, base::String> shape_files_;
  base::UnorderedMap<u16, base::String> bitmap_files_;
  base::UnorderedMap<u16, Rect> sprite_bounds_;
  base::UnorderedSet<u16> bounds_in_progress_;
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
    if (const base::String* imported = movie_.imported_symbols.find(text.font_id))
      symbol = base::StringRef(*imported);
    else if (const Font* font = movie_.FindFont(text.font_id))
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
  const base::StringRef exported = movie_.ExportName(place.character_id);
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
  const base::String* existing = shape_files_.find(shape.id);
  if (existing)
    return *existing;
  base::String file = base::Format("{}/shape_{}.svg", options_.name, shape.id);
  base::String svg = ShapeToSvg(shape);
  ExportedAsset asset;
  asset.file = file;
  for (mem_size i = 0; i < svg.size(); ++i)
    asset.bytes.push_back(static_cast<u8>(svg[i]));
  out_.assets.push_back(base::move(asset));
  shape_files_[shape.id] = file;
  return file;
}

base::String Exporter::BitmapAsset(const Bitmap& bitmap) {
  const base::String* existing = bitmap_files_.find(bitmap.id);
  if (existing)
    return *existing;
  base::String file;
  ExportedAsset asset;
  if (bitmap.is_jpeg()) {
    file = base::Format("{}/image_{}.jpg", options_.name, bitmap.id);
    asset.bytes = bitmap.jpeg;
  } else {
    file = base::Format("{}/image_{}.png", options_.name, bitmap.id);
    asset.bytes = EncodePng(bitmap.width, bitmap.height,
                            ByteSpan{bitmap.rgba.data(), bitmap.rgba.size()});
  }
  if (asset.bytes.empty())
    return base::String();
  asset.file = file;
  out_.assets.push_back(base::move(asset));
  bitmap_files_[bitmap.id] = file;
  return file;
}

Rect Exporter::CharacterBounds(u16 character_id, u32 depth) {
  if (const Shape* shape = movie_.FindShape(character_id))
    return shape->bounds;
  if (const EditText* text = movie_.FindEditText(character_id))
    return text->bounds;
  if (const StaticText* text = movie_.FindStaticText(character_id))
    return text->bounds;
  if (const Bitmap* bitmap = movie_.FindBitmap(character_id)) {
    Rect r;
    r.x_max = static_cast<i32>(bitmap->width * kTwipsPerPixel);
    r.y_max = static_cast<i32>(bitmap->height * kTwipsPerPixel);
    return r;
  }
  if (const Button* button = movie_.FindButton(character_id)) {
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
  if (const Timeline* sprite = movie_.FindSprite(character_id))
    return SpriteBounds(*sprite, depth);
  return Rect{};
}

// A sprite has no bounds of its own: it is the union of what its display list
// places, which is what a ugui panel has to be sized to.
Rect Exporter::SpriteBounds(const Timeline& timeline, u32 depth) {
  const Rect* cached = sprite_bounds_.find(timeline.id);
  if (cached)
    return *cached;
  if (depth > options_.max_depth || bounds_in_progress_.contains(timeline.id))
    return Rect{};
  bounds_in_progress_.insert(timeline.id);

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
  bounds_in_progress_.erase(timeline.id);
  sprite_bounds_[timeline.id] = out;
  return out;
}

base::String Exporter::Placement(const Box& box, const ColorTransform* color) const {
  const f32 s = options_.scale;
  base::String style = base::Format(
      "position: absolute; left: {}; top: {}; width: {}; height: {};",
      Number(box.left * s), Number(box.top * s), Number(box.width * s),
      Number(box.height * s));
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
    if (const Bitmap* bitmap = movie_.FindBitmap(bitmap_id)) {
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
    if (const Font* font = movie_.FindFont(run.font_id))
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
  if (!place.has_character || !place.visible) {
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

  if (const Shape* shape = movie_.FindShape(place.character_id)) {
    EmitShape(*shape, color, revealed, place, box, indent);
    return;
  }
  if (const EditText* text = movie_.FindEditText(place.character_id)) {
    EmitEditText(*text, color, place, box, indent);
    return;
  }
  if (const StaticText* text = movie_.FindStaticText(place.character_id)) {
    EmitStaticText(*text, color, box, indent);
    return;
  }
  if (const Bitmap* bitmap = movie_.FindBitmap(place.character_id)) {
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

  if (const Button* button = movie_.FindButton(place.character_id)) {
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

  if (const Timeline* sprite = movie_.FindSprite(place.character_id)) {
    const base::String name = NameFor(place, base::Format("sprite{}", sprite->id));
    // ugui scales a subtree uniformly; a nine-slice holds the corners while the
    // middle stretches. Note the split rather than dropping it, since a frame
    // resized the wrong way is visibly wrong.
    if (const Rect* grid = movie_.scaling_grids.find(place.character_id)) {
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
    EmitTimeline(*sprite, absolute, color, revealed, absolute_box, indent + 1,
                 depth + 1);
    Line(indent, "}");
    ++out_.widget_count;
    return;
  }

  ++out_.skipped_count;
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
  const f32 width = ToPixels(movie_.frame_size.width()) * options_.scale;
  const f32 height = ToPixels(movie_.frame_size.height()) * options_.scale;

  out_.markup += base::Format(
      "// Translated from the vanilla Scaleform movie by tools/swfdump.\n"
      "// Stage {}x{} at {:.1f} fps, {} characters, {} exported symbols.\n"
      "// Widget names are the ActionScript instance names, so the original\n"
      "// bindings still address the same objects.\n\n",
      Number(ToPixels(movie_.frame_size.width())),
      Number(ToPixels(movie_.frame_size.height())), movie_.frame_rate,
      movie_.characters.size(), movie_.exports.size());

  if (!movie_.imports.empty()) {
    out_.markup += base::Format(
        "// Imports {} symbol(s) from other movies; those subtrees are placed as\n"
        "// empty panels here because the art lives in the file named after the #:\n",
        movie_.imports.size());
    for (const base::String& entry : movie_.imports)
      out_.markup += base::Format("//   {}\n", entry);
    out_.markup += '\n';
  }

  const base::String root = UniqueName(options_.name, "screen");
  Line(0, base::Format(
              "panel {} {{ position: absolute; left: 0; top: 0; width: {}; height: {};",
              root, Number(width), Number(height)));

  Matrix identity;
  identity.translate_x = -movie_.frame_size.x_min;
  identity.translate_y = -movie_.frame_size.y_min;
  Box stage;
  stage.width = ToPixels(movie_.frame_size.width());
  stage.height = ToPixels(movie_.frame_size.height());
  EmitTimeline(movie_.root, identity, ColorTransform{}, false, stage, 1, 0);
  Line(0, "}");

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
