#ifndef RECREATION_SWF_BITMAP_H_
#define RECREATION_SWF_BITMAP_H_

#include <base/containers/vector.h>

#include "core/types.h"

namespace rx::swf {

// A decoded image character. Lossless bitmaps arrive as straight (un
// premultiplied) RGBA8; JPEG ones keep their original bytes, because the only
// consumer writes them straight back out to a file and nothing in the pipeline
// benefits from a round trip through a decoder.
struct Bitmap {
  u16 id = 0;
  u32 width = 0;
  u32 height = 0;
  base::Vector<u8> rgba;  // width*height*4, empty for JPEG characters
  base::Vector<u8> jpeg;  // original JPEG bytes, empty for lossless characters

  bool is_jpeg() const { return !jpeg.empty(); }
};

// Handles DefineBitsLossless (20), DefineBitsLossless2 (36), DefineBitsJPEG2
// (21) and DefineBitsJPEG3 (35). DefineBits (6) needs the movie's shared
// JPEGTables and is passed those bytes in `jpeg_tables`; pass an empty span for
// every other tag. Returns false when the tag is truncated or uses a bitmap
// format the spec does not define.
bool ParseBitmap(u16 tag_code, ByteSpan body, ByteSpan jpeg_tables, Bitmap& out);

// Minimal non-interlaced RGBA8 PNG. Uses the DEFLATE encoder the archive writer
// already carries, so exporting images adds no dependency.
base::Vector<u8> EncodePng(u32 width, u32 height, ByteSpan rgba);

}  // namespace rx::swf

#endif  // RECREATION_SWF_BITMAP_H_
