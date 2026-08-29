#include "components/swf/svg_export.h"

#include <base/memory/move.h>
#include <base/strings/format.h>

namespace rx::swf {
namespace {

// A SWF gradient is authored in a fixed square and placed by its matrix; the
// square runs from -16384 to 16384 twips on both axes.
constexpr f32 kGradientHalfExtent = 16384.0f / kTwipsPerPixel;

base::String FormatCoord(i32 twips) {
  const f32 px = ToPixels(twips);
  // Twips divide by 20, so two decimals are exact for every input.
  return base::Format("{:.2f}", px);
}

base::String ColorHex(Rgba c) {
  return base::Format("#{:02x}{:02x}{:02x}", static_cast<u32>(c.r), static_cast<u32>(c.g),
                      static_cast<u32>(c.b));
}

base::String Opacity(u8 alpha) {
  return base::Format("{:.3f}", static_cast<f32>(alpha) / 255.0f);
}

base::String MatrixAttribute(const Matrix& m) {
  return base::Format("matrix({:.6f} {:.6f} {:.6f} {:.6f} {:.4f} {:.4f})", m.scale_x,
                      m.rotate_skew0, m.rotate_skew1, m.scale_y,
                      ToPixels(m.translate_x), ToPixels(m.translate_y));
}

void AppendContour(const Contour& contour, base::String& out) {
  if (contour.segments.empty())
    return;
  const Segment& first = contour.segments[0];
  out += 'M';
  out += FormatCoord(first.from_x);
  out += ' ';
  out += FormatCoord(first.from_y);
  for (const Segment& s : contour.segments) {
    if (s.curved) {
      out += " Q";
      out += FormatCoord(s.control_x);
      out += ' ';
      out += FormatCoord(s.control_y);
      out += ' ';
      out += FormatCoord(s.to_x);
      out += ' ';
      out += FormatCoord(s.to_y);
    } else {
      out += " L";
      out += FormatCoord(s.to_x);
      out += ' ';
      out += FormatCoord(s.to_y);
    }
  }
  if (contour.closed)
    out += " Z";
}

void AppendGradientDef(const FillStyle& fill, u32 index, base::String& out) {
  const bool linear = fill.kind == FillKind::kLinearGradient;
  if (linear) {
    out += base::Format(
        "    <linearGradient id=\"g{}\" gradientUnits=\"userSpaceOnUse\" "
        "x1=\"{:.2f}\" y1=\"0\" x2=\"{:.2f}\" y2=\"0\" gradientTransform=\"{}\">\n",
        index, -kGradientHalfExtent, kGradientHalfExtent, MatrixAttribute(fill.matrix));
  } else {
    out += base::Format(
        "    <radialGradient id=\"g{}\" gradientUnits=\"userSpaceOnUse\" "
        "cx=\"0\" cy=\"0\" r=\"{:.2f}\" fx=\"{:.2f}\" fy=\"0\" "
        "gradientTransform=\"{}\">\n",
        index, kGradientHalfExtent, fill.focal_point * kGradientHalfExtent,
        MatrixAttribute(fill.matrix));
  }
  for (const GradientStop& stop : fill.stops) {
    out += base::Format(
        "      <stop offset=\"{:.4f}\" stop-color=\"{}\" stop-opacity=\"{}\"/>\n",
        static_cast<f32>(stop.ratio) / 255.0f, ColorHex(stop.color),
        Opacity(stop.color.a));
  }
  out += linear ? "    </linearGradient>\n" : "    </radialGradient>\n";
}

}  // namespace

bool ShapeHasVectorContent(const Shape& shape) {
  for (const StyledPath& path : shape.fill_paths) {
    if (path.style < shape.fills.size() &&
        shape.fills[path.style].kind != FillKind::kBitmap)
      return true;
  }
  return !shape.stroke_paths.empty();
}

base::String ShapeToSvg(const Shape& shape) {
  const Rect& b = shape.bounds;
  base::String out = base::Format(
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"{:.2f}\" height=\"{:.2f}\" "
      "viewBox=\"{} {} {} {}\">\n",
      ToPixels(b.width()), ToPixels(b.height()), FormatCoord(b.x_min),
      FormatCoord(b.y_min), FormatCoord(b.width()), FormatCoord(b.height()));

  bool has_gradients = false;
  for (const FillStyle& fill : shape.fills) {
    if (fill.kind == FillKind::kLinearGradient || fill.kind == FillKind::kRadialGradient ||
        fill.kind == FillKind::kFocalGradient) {
      has_gradients = true;
      break;
    }
  }
  if (has_gradients) {
    out += "  <defs>\n";
    for (u32 i = 0; i < shape.fills.size(); ++i) {
      const FillStyle& fill = shape.fills[i];
      if (fill.kind == FillKind::kLinearGradient ||
          fill.kind == FillKind::kRadialGradient || fill.kind == FillKind::kFocalGradient)
        AppendGradientDef(fill, i, out);
    }
    out += "  </defs>\n";
  }

  for (const StyledPath& path : shape.fill_paths) {
    if (path.style >= shape.fills.size())
      continue;
    const FillStyle& fill = shape.fills[path.style];
    if (fill.kind == FillKind::kBitmap)
      continue;

    base::String d;
    for (const Contour& contour : path.contours) {
      if (!d.empty())
        d += ' ';
      AppendContour(contour, d);
    }
    if (d.empty())
      continue;

    if (fill.kind == FillKind::kSolid) {
      out += base::Format(
          "  <path d=\"{}\" fill=\"{}\" fill-opacity=\"{}\" fill-rule=\"nonzero\"/>\n", d,
          ColorHex(fill.color), Opacity(fill.color.a));
    } else {
      out += base::Format(
          "  <path d=\"{}\" fill=\"url(#g{})\" fill-rule=\"nonzero\"/>\n", d, path.style);
    }
  }

  for (const StyledPath& path : shape.stroke_paths) {
    if (path.style >= shape.strokes.size())
      continue;
    const LineStyle& stroke = shape.strokes[path.style];
    base::String d;
    for (const Contour& contour : path.contours) {
      if (!d.empty())
        d += ' ';
      AppendContour(contour, d);
    }
    if (d.empty())
      continue;
    base::String paint = ColorHex(stroke.color);
    if (stroke.has_fill && stroke.fill_index < shape.fills.size()) {
      const FillStyle& fill = shape.fills[stroke.fill_index];
      paint = fill.kind == FillKind::kSolid ? ColorHex(fill.color)
                                            : base::Format("url(#g{})", stroke.fill_index);
    }
    out += base::Format(
        "  <path d=\"{}\" fill=\"none\" stroke=\"{}\" stroke-opacity=\"{}\" "
        "stroke-width=\"{:.2f}\"/>\n",
        d, paint, Opacity(stroke.color.a),
        ToPixels(stroke.width == 0 ? 20 : stroke.width));
  }

  out += "</svg>\n";
  return out;
}

}  // namespace rx::swf
