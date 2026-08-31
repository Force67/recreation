#ifndef RECREATION_SWF_SHAPE_H_
#define RECREATION_SWF_SHAPE_H_

#include <base/containers/vector.h>

#include "components/swf/types.h"
#include "core/types.h"

namespace rx::swf {

struct GradientStop {
  u8 ratio = 0;  // 0..255 along the gradient
  Rgba color;
};

enum class FillKind : u8 {
  kSolid,
  kLinearGradient,
  kRadialGradient,
  kFocalGradient,
  kBitmap,
};

struct FillStyle {
  FillKind kind = FillKind::kSolid;
  Rgba color;                        // kSolid
  base::Vector<GradientStop> stops;  // gradients
  f32 focal_point = 0;               // kFocalGradient, -1..1
  Matrix matrix;                     // gradient/bitmap space -> shape space
  u16 bitmap_id = 0;                 // kBitmap
  bool bitmap_repeat = false;
  bool bitmap_smoothed = true;
};

struct LineStyle {
  u16 width = 0;  // twips
  Rgba color;
  bool has_fill = false;  // LINESTYLE2 stroking with a fill style
  u32 fill_index = 0;     // index into Shape::fills when has_fill
};

// A quadratic segment when `curved`, a straight one otherwise. Points are
// absolute twips in shape space, so a segment can be reversed by swapping
// `from` and `to`.
struct Segment {
  i32 from_x = 0, from_y = 0;
  i32 control_x = 0, control_y = 0;
  i32 to_x = 0, to_y = 0;
  bool curved = false;
};

// One stitched contour: segments joined end to start.
struct Contour {
  base::Vector<Segment> segments;
  bool closed = false;
};

// All contours that share one style. Fills and strokes are kept apart because
// SWF draws every fill before any stroke of the same shape.
struct StyledPath {
  u32 style = 0;  // index into Shape::fills or Shape::strokes
  base::Vector<Contour> contours;
};

// A parsed DefineShape/2/3/4. Style indices are global: a shape that swaps its
// style tables mid-stream (StateNewStyles) appends to `fills`/`strokes` and the
// later records point at the appended entries, so consumers need no state.
struct Shape {
  u16 id = 0;
  Rect bounds;
  bool has_alpha = false;  // DefineShape3+ carry RGBA rather than RGB
  base::Vector<FillStyle> fills;
  base::Vector<LineStyle> strokes;
  base::Vector<StyledPath> fill_paths;
  base::Vector<StyledPath> stroke_paths;
};

// `tag_code` selects the dialect (DefineShape .. DefineShape4). Returns false on
// a truncated or malformed record; `out` is then partially filled and unusable.
bool ParseShape(u16 tag_code, ByteSpan body, Shape& out);

// A glyph outline: the same edge records as a shape, but with no style arrays
// in front of them, which is how a font's glyph table stores them. Reads from
// the reader's current position and leaves it just past the end record.
bool ParseGlyphOutline(Reader& reader, base::Vector<Contour>& out);

// True when the shape is a single axis-aligned rectangle filled with one solid
// colour and nothing else, which is what most Scaleform backing plates are.
// Such a shape becomes a ugui panel background instead of an SVG image.
bool AsSolidRect(const Shape& shape, Rgba& color);

// True when the shape is one axis-aligned rectangle filled with a single linear
// gradient. Reports the endpoint colours and the gradient angle in degrees
// (0 = left to right, growing clockwise), which maps onto ugui's
// background/background-end/gradient-angle triple.
bool AsLinearGradientRect(const Shape& shape, Rgba& start, Rgba& end, f32& angle_degrees);

// True when the shape is one rectangle filled with a single bitmap, which is
// how Flash places an imported image. Reports the bitmap character to bind.
bool AsBitmapRect(const Shape& shape, u16& bitmap_id);

// True when the shape is a click target rather than art: solid rectangles
// outlined by a stroke Flash left at an alpha of a couple of 255ths, which is
// the convention for a hit area that is never meant to be seen. A menu is full
// of them, and drawing one covers the screen it exists to catch clicks for.
bool IsHitArea(const Shape& shape);

}  // namespace rx::swf

#endif  // RECREATION_SWF_SHAPE_H_
