#include "components/swf/shape.h"

#include <base/memory/move.h>

#include <cmath>

namespace rx::swf {
namespace {

// A raw edge as it comes off the wire, before the fill/stroke regrouping. SWF
// records an edge soup: each edge names the fill to its left (fill0) and to its
// right (fill1), and contours only exist once those are followed.
struct RawEdge {
  Segment segment;
  u32 fill0 = 0;  // 0 = none, otherwise 1-based into the active fill table
  u32 fill1 = 0;
  u32 stroke = 0;  // 0 = none, otherwise 1-based
  u32 fill_base = 0;
  u32 stroke_base = 0;
};

Segment Reverse(const Segment& s) {
  Segment out = s;
  out.from_x = s.to_x;
  out.from_y = s.to_y;
  out.to_x = s.from_x;
  out.to_y = s.from_y;
  return out;
}

bool ReadFillStyle(Reader& r, bool with_alpha, bool allow_focal, FillStyle& out) {
  const u8 type = r.U8();
  switch (type) {
    case 0x00:
      out.kind = FillKind::kSolid;
      out.color = with_alpha ? r.ReadRgba() : r.ReadRgb();
      return r.ok();
    case 0x10:
    case 0x12:
    case 0x13: {
      if (type == 0x13 && !allow_focal)
        return false;
      out.kind = type == 0x10   ? FillKind::kLinearGradient
                 : type == 0x12 ? FillKind::kRadialGradient
                                : FillKind::kFocalGradient;
      out.matrix = r.ReadMatrix();
      r.Bits(2);  // spread mode
      r.Bits(2);  // interpolation mode
      const u32 count = r.Bits(4);
      for (u32 i = 0; i < count; ++i) {
        GradientStop stop;
        stop.ratio = r.U8();
        stop.color = with_alpha ? r.ReadRgba() : r.ReadRgb();
        out.stops.push_back(stop);
      }
      if (out.kind == FillKind::kFocalGradient)
        out.focal_point = r.Fixed8();
      return r.ok();
    }
    case 0x40:
    case 0x41:
    case 0x42:
    case 0x43:
      out.kind = FillKind::kBitmap;
      out.bitmap_id = r.U16();
      out.matrix = r.ReadMatrix();
      out.bitmap_repeat = (type == 0x40 || type == 0x42);
      out.bitmap_smoothed = (type == 0x40 || type == 0x41);
      return r.ok();
    default:
      return false;
  }
}

// Style arrays appear at the head of a shape and again at every StateNewStyles
// record. Both times they append to the shape's global tables; the returned
// base is what the following record-local indices are offset by.
bool ReadStyleArrays(Reader& r,
                     bool with_alpha,
                     bool shape2_plus,
                     bool shape4,
                     Shape& shape,
                     u32& fill_base,
                     u32& stroke_base) {
  fill_base = static_cast<u32>(shape.fills.size());
  stroke_base = static_cast<u32>(shape.strokes.size());

  u32 fill_count = r.U8();
  if (fill_count == 0xff && shape2_plus)
    fill_count = r.U16();
  for (u32 i = 0; i < fill_count; ++i) {
    FillStyle style;
    if (!ReadFillStyle(r, with_alpha, shape4, style))
      return false;
    shape.fills.push_back(base::move(style));
  }

  u32 line_count = r.U8();
  if (line_count == 0xff && shape2_plus)
    line_count = r.U16();
  for (u32 i = 0; i < line_count; ++i) {
    LineStyle style;
    style.width = r.U16();
    if (shape4) {
      r.Bits(2);  // start cap
      const u32 join = r.Bits(2);
      const u32 has_fill = r.Bits(1);
      r.Bits(1);  // no h scale
      r.Bits(1);  // no v scale
      r.Bits(1);  // pixel hinting
      r.Bits(5);  // reserved
      r.Bits(1);  // no close
      r.Bits(2);  // end cap
      if (join == 2)
        r.Fixed8();  // miter limit
      if (has_fill) {
        FillStyle fill;
        if (!ReadFillStyle(r, true, true, fill))
          return false;
        style.has_fill = true;
        style.fill_index = static_cast<u32>(shape.fills.size());
        shape.fills.push_back(base::move(fill));
      } else {
        style.color = r.ReadRgba();
      }
    } else {
      style.color = with_alpha ? r.ReadRgba() : r.ReadRgb();
    }
    shape.strokes.push_back(base::move(style));
  }
  return r.ok();
}

// Joins edges that share endpoints into contours. Edges arrive in draw order,
// so the greedy walk (extend the open contour with the first edge whose start
// matches its end) reproduces the authored outline for well-formed shapes and
// degrades to separate contours for the rest.
void Stitch(base::Vector<Segment>& edges, base::Vector<Contour>& out) {
  base::Vector<bool> used;
  used.resize(edges.size());
  for (mem_size i = 0; i < edges.size(); ++i)
    used[i] = false;

  for (mem_size i = 0; i < edges.size(); ++i) {
    if (used[i])
      continue;
    used[i] = true;
    Contour contour;
    contour.segments.push_back(edges[i]);
    i32 head_x = edges[i].to_x;
    i32 head_y = edges[i].to_y;
    const i32 start_x = edges[i].from_x;
    const i32 start_y = edges[i].from_y;

    bool extended = true;
    while (extended) {
      extended = false;
      if (head_x == start_x && head_y == start_y) {
        contour.closed = true;
        break;
      }
      for (mem_size j = i + 1; j < edges.size(); ++j) {
        if (used[j] || edges[j].from_x != head_x || edges[j].from_y != head_y)
          continue;
        used[j] = true;
        contour.segments.push_back(edges[j]);
        head_x = edges[j].to_x;
        head_y = edges[j].to_y;
        extended = true;
        break;
      }
    }
    if (head_x == start_x && head_y == start_y)
      contour.closed = true;
    out.push_back(base::move(contour));
  }
}

void GroupByStyle(const base::Vector<RawEdge>& edges, Shape& shape) {
  // Fills: an edge belongs to the style on its right (fill1) as drawn, and to
  // the style on its left (fill0) walked backwards, which is what closes the
  // region on that side.
  for (u32 style = 0; style < shape.fills.size(); ++style) {
    base::Vector<Segment> collected;
    for (const RawEdge& e : edges) {
      if (e.fill1 != 0 && e.fill_base + e.fill1 - 1 == style)
        collected.push_back(e.segment);
      if (e.fill0 != 0 && e.fill_base + e.fill0 - 1 == style)
        collected.push_back(Reverse(e.segment));
    }
    if (collected.empty())
      continue;
    StyledPath path;
    path.style = style;
    Stitch(collected, path.contours);
    shape.fill_paths.push_back(base::move(path));
  }

  for (u32 style = 0; style < shape.strokes.size(); ++style) {
    base::Vector<Segment> collected;
    for (const RawEdge& e : edges) {
      if (e.stroke != 0 && e.stroke_base + e.stroke - 1 == style)
        collected.push_back(e.segment);
    }
    if (collected.empty())
      continue;
    StyledPath path;
    path.style = style;
    Stitch(collected, path.contours);
    shape.stroke_paths.push_back(base::move(path));
  }
}

}  // namespace

bool ParseShape(u16 tag_code, ByteSpan body, Shape& out) {
  const bool shape2_plus = tag_code != 2;
  const bool with_alpha = tag_code == 32 || tag_code == 83;
  const bool shape4 = tag_code == 83;

  Reader r(body);
  out.id = r.U16();
  out.bounds = r.ReadRect();
  out.has_alpha = with_alpha;
  if (shape4) {
    r.ReadRect();  // edge bounds
    r.Bits(5);     // reserved
    r.Bits(1);     // uses fill winding rule
    r.Bits(1);     // uses non-scaling strokes
    r.Bits(1);     // uses scaling strokes
    r.Align();
  }

  u32 fill_base = 0;
  u32 stroke_base = 0;
  if (!ReadStyleArrays(r, with_alpha, shape2_plus, shape4, out, fill_base, stroke_base))
    return false;

  u32 fill_bits = r.Bits(4);
  u32 line_bits = r.Bits(4);

  base::Vector<RawEdge> edges;
  i32 x = 0;
  i32 y = 0;
  u32 fill0 = 0;
  u32 fill1 = 0;
  u32 stroke = 0;

  while (r.ok()) {
    if (r.Bits(1)) {
      // Edge record.
      const bool straight = r.Bits(1) != 0;
      const u32 bits = r.Bits(4) + 2;
      RawEdge edge;
      edge.fill0 = fill0;
      edge.fill1 = fill1;
      edge.stroke = stroke;
      edge.fill_base = fill_base;
      edge.stroke_base = stroke_base;
      edge.segment.from_x = x;
      edge.segment.from_y = y;
      if (straight) {
        i32 dx = 0;
        i32 dy = 0;
        if (r.Bits(1)) {
          dx = r.SignedBits(bits);
          dy = r.SignedBits(bits);
        } else if (r.Bits(1)) {
          dy = r.SignedBits(bits);
        } else {
          dx = r.SignedBits(bits);
        }
        x += dx;
        y += dy;
        edge.segment.curved = false;
      } else {
        const i32 cdx = r.SignedBits(bits);
        const i32 cdy = r.SignedBits(bits);
        const i32 adx = r.SignedBits(bits);
        const i32 ady = r.SignedBits(bits);
        edge.segment.control_x = x + cdx;
        edge.segment.control_y = y + cdy;
        x = edge.segment.control_x + adx;
        y = edge.segment.control_y + ady;
        edge.segment.curved = true;
      }
      edge.segment.to_x = x;
      edge.segment.to_y = y;
      edges.push_back(edge);
      continue;
    }

    const u32 flags = r.Bits(5);
    if (flags == 0)
      break;  // EndShapeRecord

    if (flags & 0x01) {  // StateMoveTo
      const u32 move_bits = r.Bits(5);
      x = r.SignedBits(move_bits);
      y = r.SignedBits(move_bits);
    }
    if (flags & 0x02)  // StateFillStyle0
      fill0 = r.Bits(fill_bits);
    if (flags & 0x04)  // StateFillStyle1
      fill1 = r.Bits(fill_bits);
    if (flags & 0x08)  // StateLineStyle
      stroke = r.Bits(line_bits);
    if (flags & 0x10) {  // StateNewStyles
      if (!shape2_plus)
        return false;
      r.Align();
      if (!ReadStyleArrays(r, with_alpha, shape2_plus, shape4, out, fill_base, stroke_base))
        return false;
      fill_bits = r.Bits(4);
      line_bits = r.Bits(4);
      fill0 = 0;
      fill1 = 0;
      stroke = 0;
    }
  }

  if (!r.ok())
    return false;
  GroupByStyle(edges, out);
  return true;
}

bool ParseGlyphOutline(Reader& r, base::Vector<Contour>& out) {
  // A glyph is a SHAPE, not a SHAPEWITHSTYLE: the bit widths come first and the
  // style-change records only ever select the font's one implicit fill.
  u32 fill_bits = r.Bits(4);
  u32 line_bits = r.Bits(4);

  base::Vector<Segment> edges;
  i32 x = 0;
  i32 y = 0;

  while (r.ok()) {
    if (r.Bits(1)) {
      const bool straight = r.Bits(1) != 0;
      const u32 bits = r.Bits(4) + 2;
      Segment edge;
      edge.from_x = x;
      edge.from_y = y;
      if (straight) {
        i32 dx = 0;
        i32 dy = 0;
        if (r.Bits(1)) {
          dx = r.SignedBits(bits);
          dy = r.SignedBits(bits);
        } else if (r.Bits(1)) {
          dy = r.SignedBits(bits);
        } else {
          dx = r.SignedBits(bits);
        }
        x += dx;
        y += dy;
      } else {
        const i32 cdx = r.SignedBits(bits);
        const i32 cdy = r.SignedBits(bits);
        const i32 adx = r.SignedBits(bits);
        const i32 ady = r.SignedBits(bits);
        edge.control_x = x + cdx;
        edge.control_y = y + cdy;
        x = edge.control_x + adx;
        y = edge.control_y + ady;
        edge.curved = true;
      }
      edge.to_x = x;
      edge.to_y = y;
      edges.push_back(edge);
      continue;
    }

    const u32 flags = r.Bits(5);
    if (flags == 0)
      break;
    if (flags & 0x01) {
      const u32 move_bits = r.Bits(5);
      x = r.SignedBits(move_bits);
      y = r.SignedBits(move_bits);
    }
    if (flags & 0x02)
      r.Bits(fill_bits);
    if (flags & 0x04)
      r.Bits(fill_bits);
    if (flags & 0x08)
      r.Bits(line_bits);
    if (flags & 0x10)
      return false;  // a glyph never carries new style arrays
  }
  r.Align();
  if (!r.ok())
    return false;
  Stitch(edges, out);
  return true;
}

bool AsSolidRect(const Shape& shape, Rgba& color) {
  if (shape.fills.size() != 1 || shape.fill_paths.size() != 1)
    return false;
  if (!shape.stroke_paths.empty())
    return false;
  if (shape.fills[0].kind != FillKind::kSolid)
    return false;
  const StyledPath& path = shape.fill_paths[0];
  if (path.contours.size() != 1)
    return false;
  const Contour& c = path.contours[0];
  // A rectangle is four axis-aligned straight edges, or five when the exporter
  // emits an explicit closing edge back to the start.
  if (c.segments.size() < 4 || c.segments.size() > 5)
    return false;
  for (const Segment& s : c.segments) {
    if (s.curved)
      return false;
    if (s.from_x != s.to_x && s.from_y != s.to_y)
      return false;
  }
  color = shape.fills[0].color;
  return true;
}

bool AsBitmapRect(const Shape& shape, u16& bitmap_id) {
  if (shape.fills.size() != 1 || shape.fill_paths.size() != 1)
    return false;
  if (!shape.stroke_paths.empty())
    return false;
  if (shape.fills[0].kind != FillKind::kBitmap)
    return false;
  const StyledPath& path = shape.fill_paths[0];
  if (path.contours.size() != 1)
    return false;
  const Contour& c = path.contours[0];
  if (c.segments.size() < 4 || c.segments.size() > 5)
    return false;
  for (const Segment& s : c.segments) {
    if (s.curved)
      return false;
    if (s.from_x != s.to_x && s.from_y != s.to_y)
      return false;
  }
  bitmap_id = shape.fills[0].bitmap_id;
  return true;
}

bool IsHitArea(const Shape& shape) {
  if (shape.fills.empty())
    return false;
  for (const FillStyle& fill : shape.fills) {
    if (fill.kind != FillKind::kSolid)
      return false;
  }
  // The marker: every stroke is drawn at an alpha the eye cannot resolve.
  bool marked = false;
  for (const LineStyle& stroke : shape.strokes) {
    if (stroke.color.a > 8)
      return false;
    marked = true;
  }
  if (!marked)
    return false;
  for (const StyledPath& path : shape.fill_paths) {
    for (const Contour& contour : path.contours) {
      if (contour.segments.size() < 4 || contour.segments.size() > 5)
        return false;
      for (const Segment& s : contour.segments) {
        if (s.curved)
          return false;
        if (s.from_x != s.to_x && s.from_y != s.to_y)
          return false;
      }
    }
  }
  return true;
}

bool AsLinearGradientRect(const Shape& shape, Rgba& start, Rgba& end, f32& angle_degrees) {
  if (shape.fills.size() != 1 || shape.fill_paths.size() != 1)
    return false;
  if (!shape.stroke_paths.empty())
    return false;
  const FillStyle& fill = shape.fills[0];
  if (fill.kind != FillKind::kLinearGradient || fill.stops.size() < 2)
    return false;
  const StyledPath& path = shape.fill_paths[0];
  if (path.contours.size() != 1)
    return false;
  const Contour& c = path.contours[0];
  if (c.segments.size() < 4 || c.segments.size() > 5)
    return false;
  for (const Segment& s : c.segments) {
    if (s.curved)
      return false;
    if (s.from_x != s.to_x && s.from_y != s.to_y)
      return false;
  }
  start = fill.stops[0].color;
  end = fill.stops[fill.stops.size() - 1].color;
  // A SWF linear gradient runs along the local x axis of its matrix, from
  // -16384 to 16384 twips. ugui measures gradient-angle clockwise from "down",
  // so a matrix with no rotation (gradient left to right) is 90 degrees.
  const f32 radians = std::atan2(fill.matrix.rotate_skew0, fill.matrix.scale_x);
  angle_degrees = radians * 57.29577951308232f + 90.0f;
  while (angle_degrees < 0)
    angle_degrees += 360.0f;
  while (angle_degrees >= 360.0f)
    angle_degrees -= 360.0f;
  return true;
}

}  // namespace rx::swf
