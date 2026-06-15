#include "bethesda/compression.h"

#include <cstring>

namespace rec::bethesda {
namespace {

// ---------------------------------------------------------------------------
// DEFLATE (RFC 1951), in the spirit of zlib's puff.c: canonical Huffman
// decoding straight from code lengths, no tables beyond counts and symbols.

struct BitReader {
  const u8* data;
  size_t size;
  size_t pos = 0;   // byte position
  u32 bit_buffer = 0;
  int bit_count = 0;

  // Returns -1 past the end of input.
  int Bits(int need) {
    while (bit_count < need) {
      if (pos >= size) return -1;
      bit_buffer |= static_cast<u32>(data[pos++]) << bit_count;
      bit_count += 8;
    }
    int value = static_cast<int>(bit_buffer & ((1u << need) - 1));
    bit_buffer >>= need;
    bit_count -= need;
    return value;
  }
};

struct Huffman {
  u16 count[16];    // codes per bit length
  u16 symbol[288];  // symbols ordered canonically
};

bool BuildHuffman(const u8* lengths, int n, Huffman* h) {
  std::memset(h->count, 0, sizeof(h->count));
  for (int i = 0; i < n; ++i) ++h->count[lengths[i]];
  if (h->count[0] == n) return true;  // no codes, caller decides if that is ok

  int left = 1;  // over-subscription check
  for (int len = 1; len < 16; ++len) {
    left <<= 1;
    left -= h->count[len];
    if (left < 0) return false;
  }

  u16 offsets[16];
  offsets[1] = 0;
  for (int len = 1; len < 15; ++len) offsets[len + 1] = static_cast<u16>(offsets[len] + h->count[len]);
  for (int i = 0; i < n; ++i) {
    if (lengths[i] != 0) h->symbol[offsets[lengths[i]]++] = static_cast<u16>(i);
  }
  return true;
}

int DecodeSymbol(BitReader& in, const Huffman& h) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len < 16; ++len) {
    int bit = in.Bits(1);
    if (bit < 0) return -1;
    code |= bit;
    int count = h.count[len];
    if (code - first < count) return h.symbol[index + (code - first)];
    index += count;
    first = (first + count) << 1;
    code <<= 1;
  }
  return -1;
}

struct Output {
  u8* dst;
  size_t size;
  size_t pos = 0;
};

bool InflateBlock(BitReader& in, Output& out, const Huffman& lit, const Huffman& dist) {
  static constexpr u16 kLengthBase[] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19, 23, 27,
                                        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static constexpr u8 kLengthExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                        2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static constexpr u16 kDistBase[] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                      33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                      1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
  static constexpr u8 kDistExtra[] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,  4,  4,  5,  5,  6,
                                      6, 7, 7,  8,  8,  9,  9,  10, 10, 11, 11, 12, 12, 13, 13};

  for (;;) {
    int symbol = DecodeSymbol(in, lit);
    if (symbol < 0) return false;
    if (symbol < 256) {
      if (out.pos >= out.size) return false;
      out.dst[out.pos++] = static_cast<u8>(symbol);
    } else if (symbol == 256) {
      return true;
    } else {
      symbol -= 257;
      if (symbol >= 29) return false;
      int extra = in.Bits(kLengthExtra[symbol]);
      if (extra < 0) return false;
      size_t length = kLengthBase[symbol] + static_cast<size_t>(extra);

      int dsym = DecodeSymbol(in, dist);
      if (dsym < 0 || dsym >= 30) return false;
      extra = in.Bits(kDistExtra[dsym]);
      if (extra < 0) return false;
      size_t distance = kDistBase[dsym] + static_cast<size_t>(extra);
      if (distance > out.pos || out.pos + length > out.size) return false;
      for (size_t i = 0; i < length; ++i, ++out.pos) out.dst[out.pos] = out.dst[out.pos - distance];
    }
  }
}

bool InflateFixed(BitReader& in, Output& out) {
  u8 lengths[288];
  for (int i = 0; i < 144; ++i) lengths[i] = 8;
  for (int i = 144; i < 256; ++i) lengths[i] = 9;
  for (int i = 256; i < 280; ++i) lengths[i] = 7;
  for (int i = 280; i < 288; ++i) lengths[i] = 8;
  Huffman lit;
  if (!BuildHuffman(lengths, 288, &lit)) return false;
  for (int i = 0; i < 30; ++i) lengths[i] = 5;
  Huffman dist;
  if (!BuildHuffman(lengths, 30, &dist)) return false;
  return InflateBlock(in, out, lit, dist);
}

bool InflateDynamic(BitReader& in, Output& out) {
  static constexpr u8 kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

  int hlit = in.Bits(5);
  int hdist = in.Bits(5);
  int hclen = in.Bits(4);
  if (hlit < 0 || hdist < 0 || hclen < 0) return false;
  int nlit = hlit + 257, ndist = hdist + 1, ncode = hclen + 4;

  u8 lengths[320] = {};
  for (int i = 0; i < ncode; ++i) {
    int bits = in.Bits(3);
    if (bits < 0) return false;
    lengths[kOrder[i]] = static_cast<u8>(bits);
  }
  for (int i = ncode; i < 19; ++i) lengths[kOrder[i]] = 0;
  Huffman code_huffman;
  if (!BuildHuffman(lengths, 19, &code_huffman)) return false;

  std::memset(lengths, 0, sizeof(lengths));
  int index = 0;
  while (index < nlit + ndist) {
    int symbol = DecodeSymbol(in, code_huffman);
    if (symbol < 0) return false;
    if (symbol < 16) {
      lengths[index++] = static_cast<u8>(symbol);
    } else {
      int repeat;
      u8 value = 0;
      if (symbol == 16) {
        if (index == 0) return false;
        value = lengths[index - 1];
        repeat = in.Bits(2);
        if (repeat < 0) return false;
        repeat += 3;
      } else if (symbol == 17) {
        repeat = in.Bits(3);
        if (repeat < 0) return false;
        repeat += 3;
      } else {
        repeat = in.Bits(7);
        if (repeat < 0) return false;
        repeat += 11;
      }
      if (index + repeat > nlit + ndist) return false;
      while (repeat-- > 0) lengths[index++] = value;
    }
  }

  Huffman lit, dist;
  if (!BuildHuffman(lengths, nlit, &lit)) return false;
  if (!BuildHuffman(lengths + nlit, ndist, &dist)) return false;
  return InflateBlock(in, out, lit, dist);
}

bool InflateStored(BitReader& in, Output& out) {
  in.bit_buffer = 0;
  in.bit_count = 0;  // byte align
  if (in.pos + 4 > in.size) return false;
  u16 length = static_cast<u16>(in.data[in.pos] | in.data[in.pos + 1] << 8);
  in.pos += 4;  // length + one's complement
  if (in.pos + length > in.size || out.pos + length > out.size) return false;
  std::memcpy(out.dst + out.pos, in.data + in.pos, length);
  in.pos += length;
  out.pos += length;
  return true;
}

bool Inflate(ByteSpan src, u8* dst, size_t dst_size) {
  BitReader in{src.data(), src.size()};
  Output out{dst, dst_size};
  for (;;) {
    int last = in.Bits(1);
    int type = in.Bits(2);
    if (last < 0 || type < 0) return false;
    bool ok = false;
    switch (type) {
      case 0: ok = InflateStored(in, out); break;
      case 1: ok = InflateFixed(in, out); break;
      case 2: ok = InflateDynamic(in, out); break;
      default: return false;
    }
    if (!ok) return false;
    if (last) break;
  }
  return out.pos == dst_size;
}

// ---------------------------------------------------------------------------
// LZ4

bool Lz4Block(const u8* src, size_t src_size, u8* dst, size_t dst_capacity, size_t* dst_pos) {
  size_t i = 0;
  size_t pos = *dst_pos;
  while (i < src_size) {
    u8 token = src[i++];
    size_t literal = token >> 4;
    if (literal == 15) {
      u8 b;
      do {
        if (i >= src_size) return false;
        b = src[i++];
        literal += b;
      } while (b == 255);
    }
    if (i + literal > src_size || pos + literal > dst_capacity) return false;
    std::memcpy(dst + pos, src + i, literal);
    i += literal;
    pos += literal;
    if (i >= src_size) break;  // last sequence has no match

    if (i + 2 > src_size) return false;
    size_t offset = src[i] | static_cast<size_t>(src[i + 1]) << 8;
    i += 2;
    size_t match = token & 15;
    if (match == 15) {
      u8 b;
      do {
        if (i >= src_size) return false;
        b = src[i++];
        match += b;
      } while (b == 255);
    }
    match += 4;
    if (offset == 0 || offset > pos || pos + match > dst_capacity) return false;
    // Byte-wise on purpose: overlapping matches are the RLE case.
    for (size_t k = 0; k < match; ++k, ++pos) dst[pos] = dst[pos - offset];
  }
  *dst_pos = pos;
  return true;
}

}  // namespace

bool ZlibInflate(ByteSpan src, u8* dst, size_t dst_size) {
  if (src.size() < 2) return false;
  // 2 byte zlib header, deflate stream, 4 byte adler32 we do not verify.
  if ((src[0] & 0x0f) != 8) return false;
  return Inflate(src.subspan(2), dst, dst_size);
}

bool Lz4FrameDecompress(ByteSpan src, u8* dst, size_t dst_size) {
  constexpr u32 kMagic = 0x184d2204;
  if (src.size() < 7) return false;
  u32 magic;
  std::memcpy(&magic, src.data(), 4);
  if (magic != kMagic) return false;

  u8 flg = src[4];
  bool has_content_size = (flg >> 3) & 1;
  bool block_checksums = (flg >> 4) & 1;
  bool content_checksum = (flg >> 2) & 1;
  bool has_dict_id = flg & 1;
  size_t pos = 6;  // FLG + BD
  if (has_content_size) pos += 8;
  if (has_dict_id) pos += 4;
  pos += 1;  // header checksum

  size_t out_pos = 0;
  for (;;) {
    if (pos + 4 > src.size()) return false;
    u32 block_size;
    std::memcpy(&block_size, src.data() + pos, 4);
    pos += 4;
    if (block_size == 0) break;  // end mark
    bool uncompressed = block_size & 0x80000000u;
    block_size &= 0x7fffffffu;
    if (pos + block_size > src.size()) return false;
    if (uncompressed) {
      if (out_pos + block_size > dst_size) return false;
      std::memcpy(dst + out_pos, src.data() + pos, block_size);
      out_pos += block_size;
    } else {
      // Blocks may be linked, matches see the whole output so far.
      if (!Lz4Block(src.data() + pos, block_size, dst, dst_size, &out_pos)) return false;
    }
    pos += block_size;
    if (block_checksums) pos += 4;
  }
  (void)content_checksum;
  return out_pos == dst_size;
}

}  // namespace rec::bethesda
