#ifndef RECREATION_BETHESDA_COMPRESSION_H_
#define RECREATION_BETHESDA_COMPRESSION_H_

#include <base/containers/vector.h>

#include "core/types.h"

namespace rx::bethesda {

// Self contained decoders for the two codecs bethesda archives and plugins
// use. The build has no zlib/lz4 dependency, the formats are small enough to
// carry ourselves. Both need the exact output size, which the containers
// store next to the compressed bytes. Checksums (adler32, xxhash) are not
// verified.

// Raw DEFLATE stream wrapped in a zlib header (RFC 1950/1951). Compressed
// plugin records and BSA v104 files use this.
bool ZlibInflate(ByteSpan src, u8* dst, size_t dst_size);

// Wraps `src` in a zlib stream (RFC 1950) built from stored (uncompressed)
// DEFLATE blocks, with a valid header and adler32. Any zlib decoder accepts it,
// including ZlibInflate and the shipping game's zlib. It does not shrink the
// data (a real compressor is future work); it exists so the writer can emit
// spec-correct compressed records.
base::Vector<u8> ZlibDeflateStored(ByteSpan src);

// Wraps `src` in a zlib stream (RFC 1950) that actually compresses: LZ77 with a
// hash-chain match finder (32K window) feeding fixed-Huffman DEFLATE blocks
// (BTYPE=01, RFC 1951 3.2.6). Decodable by ZlibInflate and any zlib decoder.
// Produces a valid header and adler32; use this instead of ZlibDeflateStored
// when the goal is to shrink the data.
base::Vector<u8> ZlibDeflate(ByteSpan src);

// LZ4 frame format, including linked blocks. BSA v105 (Skyrim SE) files use
// this.
bool Lz4FrameDecompress(ByteSpan src, u8* dst, size_t dst_size);

// A single raw LZ4 block (no frame header). Starfield v3 BA2 texture chunks are
// stored this way. dst_size must be the exact decompressed size.
bool Lz4BlockDecompress(ByteSpan src, u8* dst, size_t dst_size);

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_COMPRESSION_H_
