#ifndef RECREATION_SWF_SVG_EXPORT_H_
#define RECREATION_SWF_SVG_EXPORT_H_

#include <base/strings/xstring.h>

#include "components/swf/shape.h"

namespace rx::swf {

// Writes a shape out as a standalone SVG document, in pixels, with the shape's
// own bounds as the viewBox. Solid and gradient fills survive exactly; strokes
// become stroked paths.
//
// libultragui rasterizes SVG itself, so this is how the movie's vector art
// reaches the rebuilt UI without ever being flattened to a screenshot: the
// curves stay curves and re-render crisply at any scale.
//
// Bitmap fills have no SVG equivalent that ugui's rasterizer reads, so a path
// filled with one is skipped here; `AsBitmapRect` recognises that case and the
// ugui exporter places the bitmap as an image widget instead.
base::String ShapeToSvg(const Shape& shape);

// True when the shape has any drawable content once bitmap fills are excluded.
bool ShapeHasVectorContent(const Shape& shape);

}  // namespace rx::swf

#endif  // RECREATION_SWF_SVG_EXPORT_H_
