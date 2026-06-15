#ifndef RECREATION_BETHESDA_COMPRESSION_H_
#define RECREATION_BETHESDA_COMPRESSION_H_

#include "core/types.h"

namespace rec::bethesda {

// Self contained decoders for the two codecs bethesda archives and plugins
// use. The build has no zlib/lz4 dependency, the formats are small enough to
// carry ourselves. Both need the exact output size, which the containers
// store next to the compressed bytes. Checksums (adler32, xxhash) are not
// verified.

// Raw DEFLATE stream wrapped in a zlib header (RFC 1950/1951). Compressed
// plugin records and BSA v104 files use this.
bool ZlibInflate(ByteSpan src, u8* dst, size_t dst_size);

// LZ4 frame format, including linked blocks. BSA v105 (Skyrim SE) files use
// this.
bool Lz4FrameDecompress(ByteSpan src, u8* dst, size_t dst_size);

}  // namespace rec::bethesda

#endif  // RECREATION_BETHESDA_COMPRESSION_H_
