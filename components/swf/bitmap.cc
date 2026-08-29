#include "components/swf/bitmap.h"

#include <base/memory/move.h>

#include <cstring>

#include "components/bethesda/compression.h"
#include "components/swf/types.h"

namespace rx::swf {
namespace {

constexpr u32 kMaxDimension = 8192;

// Rows of an indexed or 15-bit lossless bitmap are padded out to a 32-bit
// boundary; 24/32-bit rows never need it.
u32 PaddedRowBytes(u32 bytes) {
  return (bytes + 3u) & ~3u;
}

void Unpremultiply(u8* rgba, u32 pixels) {
  for (u32 i = 0; i < pixels; ++i) {
    const u32 a = rgba[i * 4 + 3];
    if (a == 0 || a == 255)
      continue;
    for (u32 c = 0; c < 3; ++c) {
      const u32 v = rgba[i * 4 + c] * 255u / a;
      rgba[i * 4 + c] = static_cast<u8>(v > 255 ? 255 : v);
    }
  }
}

bool DecodeLossless(Reader& r, bool with_alpha, ByteSpan body, Bitmap& out) {
  const u8 format = r.U8();
  out.width = r.U16();
  out.height = r.U16();
  if (!r.ok() || out.width == 0 || out.height == 0 || out.width > kMaxDimension ||
      out.height > kMaxDimension)
    return false;

  u32 table_size = 0;
  if (format == 3)
    table_size = static_cast<u32>(r.U8()) + 1;
  else if (format != 4 && format != 5)
    return false;
  if (format == 4 && with_alpha)
    return false;  // 15-bit has no alpha variant

  const u32 entry_bytes = with_alpha ? 4u : 3u;
  u32 row_bytes = 0;
  switch (format) {
    case 3:
      row_bytes = PaddedRowBytes(out.width);
      break;
    case 4:
      row_bytes = PaddedRowBytes(out.width * 2);
      break;
    default:
      row_bytes = out.width * 4;
      break;
  }
  const u32 raw_size = table_size * entry_bytes + row_bytes * out.height;

  base::Vector<u8> raw;
  raw.resize(raw_size);
  const ByteSpan compressed = body.subspan(r.pos());
  if (!bethesda::ZlibInflate(compressed, raw.data(), raw_size))
    return false;

  out.rgba.resize(static_cast<mem_size>(out.width) * out.height * 4);
  u8* dst = out.rgba.data();
  const u8* pixels = raw.data() + table_size * entry_bytes;

  for (u32 y = 0; y < out.height; ++y) {
    const u8* row = pixels + static_cast<mem_size>(y) * row_bytes;
    for (u32 x = 0; x < out.width; ++x) {
      u8* p = dst + (static_cast<mem_size>(y) * out.width + x) * 4;
      if (format == 3) {
        const u32 index = row[x];
        if (index >= table_size) {
          p[0] = p[1] = p[2] = p[3] = 0;
          continue;
        }
        const u8* entry = raw.data() + index * entry_bytes;
        p[0] = entry[0];
        p[1] = entry[1];
        p[2] = entry[2];
        p[3] = with_alpha ? entry[3] : 255;
      } else if (format == 4) {
        const u16 v = static_cast<u16>(row[x * 2] << 8 | row[x * 2 + 1]);
        p[0] = static_cast<u8>(((v >> 10) & 0x1f) * 255 / 31);
        p[1] = static_cast<u8>(((v >> 5) & 0x1f) * 255 / 31);
        p[2] = static_cast<u8>((v & 0x1f) * 255 / 31);
        p[3] = 255;
      } else {
        // PIX24 is (reserved|alpha, red, green, blue) big-endian.
        p[0] = row[x * 4 + 1];
        p[1] = row[x * 4 + 2];
        p[2] = row[x * 4 + 3];
        p[3] = with_alpha ? row[x * 4 + 0] : 255;
      }
    }
  }
  if (with_alpha && format == 5)
    Unpremultiply(out.rgba.data(), out.width * out.height);
  return true;
}

// Flash writes an erroneous end-of-image/start-of-image pair at the head of
// some embedded streams. Decoders are expected to drop it.
ByteSpan StripErroneousMarker(ByteSpan jpeg) {
  if (jpeg.size() >= 4 && jpeg[0] == 0xff && jpeg[1] == 0xd9 && jpeg[2] == 0xff &&
      jpeg[3] == 0xd8)
    return jpeg.subspan(4);
  return jpeg;
}

void AppendBe32(base::Vector<u8>& out, u32 v) {
  out.push_back(static_cast<u8>(v >> 24));
  out.push_back(static_cast<u8>(v >> 16));
  out.push_back(static_cast<u8>(v >> 8));
  out.push_back(static_cast<u8>(v));
}

u32 Crc32(const u8* data, mem_size size) {
  static u32 table[256];
  static bool built = false;
  if (!built) {
    for (u32 i = 0; i < 256; ++i) {
      u32 c = i;
      for (int k = 0; k < 8; ++k)
        c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    built = true;
  }
  u32 crc = 0xffffffffu;
  for (mem_size i = 0; i < size; ++i)
    crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
  return crc ^ 0xffffffffu;
}

void AppendChunk(base::Vector<u8>& out, const char type[4], const base::Vector<u8>& data) {
  AppendBe32(out, static_cast<u32>(data.size()));
  base::Vector<u8> crc_input;
  for (int i = 0; i < 4; ++i)
    crc_input.push_back(static_cast<u8>(type[i]));
  for (mem_size i = 0; i < data.size(); ++i)
    crc_input.push_back(data[i]);
  for (mem_size i = 0; i < crc_input.size(); ++i)
    out.push_back(crc_input[i]);
  AppendBe32(out, Crc32(crc_input.data(), crc_input.size()));
}

}  // namespace

bool ParseBitmap(u16 tag_code, ByteSpan body, ByteSpan jpeg_tables, Bitmap& out) {
  Reader r(body);
  out.id = r.U16();
  if (!r.ok())
    return false;

  switch (tag_code) {
    case 20:
      return DecodeLossless(r, false, body, out);
    case 36:
      return DecodeLossless(r, true, body, out);
    case 6: {
      // DefineBits carries only the scan data; the tables live in the movie's
      // one JPEGTables tag. Splice them by dropping the trailing EOI of the
      // tables and the leading SOI of the scan.
      const ByteSpan scan = StripErroneousMarker(r.Rest());
      if (jpeg_tables.size() < 2 || scan.size() < 2)
        return false;
      const mem_size head = jpeg_tables.size() - 2;  // strip FFD9
      for (mem_size i = 0; i < head; ++i)
        out.jpeg.push_back(jpeg_tables[i]);
      for (mem_size i = 2; i < scan.size(); ++i)  // strip FFD8
        out.jpeg.push_back(scan[i]);
      return true;
    }
    case 21: {
      const ByteSpan jpeg = StripErroneousMarker(r.Rest());
      for (mem_size i = 0; i < jpeg.size(); ++i)
        out.jpeg.push_back(jpeg[i]);
      return !out.jpeg.empty();
    }
    case 35: {
      const u32 alpha_offset = r.U32();
      if (!r.ok() || alpha_offset > r.remaining())
        return false;
      const ByteSpan jpeg = StripErroneousMarker(r.Bytes(alpha_offset));
      for (mem_size i = 0; i < jpeg.size(); ++i)
        out.jpeg.push_back(jpeg[i]);
      // The zlib alpha plane needs the image dimensions to size, which only a
      // JPEG decoder knows. Keeping the colour data is the useful half; the
      // alpha is dropped rather than guessed at.
      return !out.jpeg.empty();
    }
    default:
      return false;
  }
}

base::Vector<u8> EncodePng(u32 width, u32 height, ByteSpan rgba) {
  base::Vector<u8> out;
  if (width == 0 || height == 0 ||
      rgba.size() < static_cast<mem_size>(width) * height * 4)
    return out;

  const u8 signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  for (u8 b : signature)
    out.push_back(b);

  base::Vector<u8> ihdr;
  AppendBe32(ihdr, width);
  AppendBe32(ihdr, height);
  ihdr.push_back(8);  // bit depth
  ihdr.push_back(6);  // colour type: truecolour with alpha
  ihdr.push_back(0);  // deflate
  ihdr.push_back(0);  // adaptive filtering
  ihdr.push_back(0);  // no interlace
  AppendChunk(out, "IHDR", ihdr);

  // Filter type 0 (none) per scanline: the DEFLATE encoder does the work and
  // predictors would only pay off for photographic data.
  base::Vector<u8> raw;
  raw.resize(static_cast<mem_size>(height) * (static_cast<mem_size>(width) * 4 + 1));
  for (u32 y = 0; y < height; ++y) {
    u8* row = raw.data() + static_cast<mem_size>(y) * (width * 4 + 1);
    row[0] = 0;
    std::memcpy(row + 1, rgba.data() + static_cast<mem_size>(y) * width * 4, width * 4);
  }
  base::Vector<u8> idat =
      bethesda::ZlibDeflate(ByteSpan{raw.data(), raw.size()});
  AppendChunk(out, "IDAT", idat);

  base::Vector<u8> iend;
  AppendChunk(out, "IEND", iend);
  return out;
}

}  // namespace rx::swf
