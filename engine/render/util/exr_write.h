#ifndef RECREATION_RENDER_EXR_WRITE_H_
#define RECREATION_RENDER_EXR_WRITE_H_

#include <string>

#include "core/types.h"

namespace rec::render {

// Writes a 3-channel (RGB) 32-bit float, uncompressed, scanline OpenEXR. `rgb`
// is width*height*3 floats, row-major, top row first. Returns false on a file
// error. Minimal but spec-correct: enough for compositors (Nuke, Resolve, oiio,
// ffmpeg) to read the engine's linear-hdr captures as a production container.
bool WriteExrRgbF32(const std::string& path, u32 width, u32 height, const f32* rgb);

}  // namespace rec::render

#endif  // RECREATION_RENDER_EXR_WRITE_H_
