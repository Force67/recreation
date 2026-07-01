#include "bethesda/compression.h"

#include <algorithm>
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

bool Lz4BlockDecompress(ByteSpan src, u8* dst, size_t dst_size) {
  size_t out_pos = 0;
  if (!Lz4Block(src.data(), src.size(), dst, dst_size, &out_pos)) return false;
  return out_pos == dst_size;
}

base::Vector<u8> ZlibDeflateStored(ByteSpan src) {
  // Stored blocks have an exactly known size, so size the buffer once and write
  // through the data pointer (no push_back growth path, which GCC's
  // -Wstringop-overflow mis-analyzes on this container).
  size_t blocks = src.empty() ? 1 : (src.size() + 0xfffe) / 0xffff;
  base::Vector<u8> out;
  out.resize(2 + 5 * blocks + src.size() + 4);
  u8* p = out.data();
  size_t w = 0;

  // zlib header: CMF 0x78 (32K window, deflate), FLG 0x01 so (CMF<<8|FLG)%31==0.
  p[w++] = 0x78;
  p[w++] = 0x01;

  size_t pos = 0;
  do {
    size_t chunk = std::min<size_t>(src.size() - pos, 0xffff);
    bool last = pos + chunk == src.size();
    p[w++] = last ? 0x01 : 0x00;  // BFINAL bit, BTYPE 00 (stored)
    u16 len = static_cast<u16>(chunk);
    u16 nlen = static_cast<u16>(~len);
    p[w++] = static_cast<u8>(len);
    p[w++] = static_cast<u8>(len >> 8);
    p[w++] = static_cast<u8>(nlen);
    p[w++] = static_cast<u8>(nlen >> 8);
    if (chunk) std::memcpy(p + w, src.data() + pos, chunk);
    w += chunk;
    pos += chunk;
  } while (pos < src.size());

  // adler32 of the uncompressed data, big endian.
  u32 a = 1, b = 0;
  for (u8 c : src) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  u32 adler = (b << 16) | a;
  p[w++] = static_cast<u8>(adler >> 24);
  p[w++] = static_cast<u8>(adler >> 16);
  p[w++] = static_cast<u8>(adler >> 8);
  p[w++] = static_cast<u8>(adler);
  return out;
}

namespace {

// ---------------------------------------------------------------------------
// DEFLATE encoder: LZ77 (hash-chain match finder) tokenised once, then entropy
// coded as whichever of {stored, fixed-Huffman, dynamic-Huffman} is smallest.
// The bit packing mirrors the decoder's BitReader exactly: bytes are filled
// LSB-first, extra bits go out LSB-first, and Huffman codes go out MSB-first
// (RFC 1951 3.1.1), which we get by reversing the canonical code's bits before
// feeding them to the LSB-first writer.

// Same base/extra tables the decoder uses, indexed by symbol - 257 (length) and
// distance symbol respectively.
constexpr u16 kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19, 23, 27,
                              31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr u8 kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                              2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr u16 kDistBaseE[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,    25,
                                33,   49,   65,   97,   129,  193,   257,   385,   513,   769,
                                1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
constexpr u8 kDistExtraE[30] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,  4,  4,  5,  5,  6,
                                6, 7, 7,  8,  8,  9,  9,  10, 10, 11, 11, 12, 12, 13, 13};

// Reverse the low `len` bits of `code` (canonical codes are defined MSB-first).
u32 ReverseBits(u32 code, int len) {
  u32 r = 0;
  for (int i = 0; i < len; ++i) {
    r = (r << 1) | (code & 1u);
    code >>= 1;
  }
  return r;
}

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif

// Length symbol index (0..28) for a match length 3..258, and distance symbol
// index (0..29) for a distance 1..32768. Both pick the largest base <= value,
// matching the decoder's kLengthBase / kDistBase tables exactly.
int LenSymIndex(int length) {
  int li = 0;
  while (li < 28 && kLenBase[li + 1] <= length) ++li;
  return li;
}
int DistSymIndex(u32 distance) {
  int di = 0;
  while (di < 29 && kDistBaseE[di + 1] <= distance) ++di;
  return di;
}

// Order in which the code-length-code lengths are transmitted (RFC 1951 3.2.7);
// identical to the decoder's kOrder.
constexpr u8 kCodeLenOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

// A tokenised LZ77 element. dist == 0 marks a literal (sym holds the byte);
// otherwise it is a back-reference (sym holds the length 3..258).
struct Token {
  u16 sym;
  u16 dist;
};

// Assigns canonical (MSB-first, RFC 1951 3.1.1) codes from code lengths; symbols
// with length 0 get no code. This is the exact inverse of the decoder's
// canonical assignment (BuildHuffman/DecodeSymbol), so codes made here decode.
void BuildCanonicalCodes(const u8* len, int n, u32* codes) {
  int bl_count[16] = {0};
  for (int i = 0; i < n; ++i) ++bl_count[len[i]];
  bl_count[0] = 0;
  u32 next[16] = {0};
  u32 code = 0;
  for (int bits = 1; bits < 16; ++bits) {
    code = (code + static_cast<u32>(bl_count[bits - 1])) << 1;
    next[bits] = code;
  }
  for (int i = 0; i < n; ++i) {
    if (len[i]) codes[i] = next[len[i]]++;
  }
}

// Optimal length-limited Huffman code lengths via the package-merge algorithm
// (Larmore-Hirschberg). freq[i] > 0 marks a used symbol; the caller guarantees
// at least two used symbols. Writes lengths in [1, maxlen] for used symbols and
// 0 elsewhere. The result is a complete code (Kraft equality holds) because
// maxlen >= ceil(log2(#used)) for every alphabet used here.
void PackageMerge(const u32* freq, int n, int maxlen, u8* out_len) {
  base::Vector<int> sym;
  base::Vector<u64> wt;
  for (int i = 0; i < n; ++i) {
    if (freq[i] > 0) {
      sym.push_back(i);
      wt.push_back(freq[i]);
    }
  }
  const int m = static_cast<int>(sym.size());

  // Sort active symbols by ascending weight (insertion sort; m is small).
  base::Vector<int> ord;
  for (int i = 0; i < m; ++i) ord.push_back(i);
  for (int i = 1; i < m; ++i) {
    int key = ord[i];
    int j = i - 1;
    while (j >= 0 && wt[ord[j]] > wt[key]) {
      ord[j + 1] = ord[j];
      --j;
    }
    ord[j + 1] = key;
  }

  // Original coin list: node k covers active symbol oa[k] with weight ow[k].
  base::Vector<u64> ow;
  base::Vector<int> oa;
  for (int k = 0; k < m; ++k) {
    ow.push_back(wt[ord[k]]);
    oa.push_back(ord[k]);
  }

  // The "current" list; each node carries per-active-symbol coin counts,
  // flattened as node * m + active_index. It starts equal to the original list.
  int csz = m;
  base::Vector<u64> cw = ow;
  base::Vector<u32> cc;
  cc.resize(static_cast<size_t>(m) * static_cast<size_t>(m), 0);
  for (int k = 0; k < m; ++k) cc[static_cast<size_t>(k) * m + oa[k]] = 1;

  for (int iter = 0; iter < maxlen - 1; ++iter) {
    const int pcount = csz / 2;
    base::Vector<u64> pw;
    pw.resize(pcount);
    base::Vector<u32> pc;
    pc.resize(static_cast<size_t>(pcount) * static_cast<size_t>(m), 0);
    for (int i = 0; i < pcount; ++i) {
      pw[i] = cw[2 * i] + cw[2 * i + 1];
      for (int a = 0; a < m; ++a) {
        pc[static_cast<size_t>(i) * m + a] = cc[static_cast<size_t>(2 * i) * m + a] +
                                             cc[static_cast<size_t>(2 * i + 1) * m + a];
      }
    }
    // Merge the original list with the packaged list (both ascending by weight).
    const int newsz = m + pcount;
    base::Vector<u64> nw;
    nw.resize(newsz);
    base::Vector<u32> nc;
    nc.resize(static_cast<size_t>(newsz) * static_cast<size_t>(m), 0);
    int io = 0, ip = 0, k = 0;
    while (io < m || ip < pcount) {
      bool take_orig;
      if (io >= m)
        take_orig = false;
      else if (ip >= pcount)
        take_orig = true;
      else
        take_orig = ow[io] <= pw[ip];
      if (take_orig) {
        nw[k] = ow[io];
        nc[static_cast<size_t>(k) * m + oa[io]] = 1;
        ++io;
      } else {
        nw[k] = pw[ip];
        for (int a = 0; a < m; ++a)
          nc[static_cast<size_t>(k) * m + a] = pc[static_cast<size_t>(ip) * m + a];
        ++ip;
      }
      ++k;
    }
    cw = nw;
    cc = nc;
    csz = newsz;
  }

  // The code length of a symbol is how many of the 2m-2 smallest coins cover it.
  int select = 2 * m - 2;
  if (select > csz) select = csz;
  base::Vector<u32> length;
  length.resize(m, 0);
  for (int k = 0; k < select; ++k) {
    for (int a = 0; a < m; ++a) length[a] += cc[static_cast<size_t>(k) * m + a];
  }
  for (int a = 0; a < m; ++a) out_len[sym[a]] = static_cast<u8>(length[a]);
}

// Builds decoder-ready code lengths for an alphabet of size n. Always yields a
// complete code with at least two codes (like zlib): an unused alphabet (the
// no-distance case) gets two 1-bit codes, and a single-symbol alphabet gets its
// code plus one phantom so the lone code is not left incomplete. Otherwise it
// defers to package-merge for the optimal length-limited assignment.
void BuildTree(const u32* freq, int n, int maxlen, u8* out_len) {
  for (int i = 0; i < n; ++i) out_len[i] = 0;
  int used = 0, first = -1;
  for (int i = 0; i < n; ++i) {
    if (freq[i] > 0) {
      ++used;
      if (first < 0) first = i;
    }
  }
  if (used == 0) {
    out_len[0] = 1;
    out_len[1] = 1;
    return;
  }
  if (used == 1) {
    out_len[first] = 1;
    out_len[first == 0 ? 1 : 0] = 1;
    return;
  }
  PackageMerge(freq, n, maxlen, out_len);
}

// One emitted code-length-code: `sym` is the RLE symbol (0..18), optionally
// followed by `nbits` extra value bits `val` (written LSB-first, not a code).
struct RleItem {
  u8 sym;
  u8 nbits;
  u16 val;
};

// Run-length encodes the concatenated lit/len + distance code lengths using the
// code-length alphabet (RFC 1951 3.2.7): 16 copies the previous length 3..6
// times, 17 emits 3..10 zeros, 18 emits 11..138 zeros. Fills the item list and
// the code-length-symbol histogram, matching exactly what InflateDynamic reads.
void RleCodeLengths(const u8* cl, int total, base::Vector<RleItem>& items, u32* freq_cl) {
  for (int i = 0; i < 19; ++i) freq_cl[i] = 0;
  int i = 0;
  while (i < total) {
    u8 cur = cl[i];
    int run = 1;
    while (i + run < total && cl[i + run] == cur) ++run;
    if (cur == 0) {
      while (run >= 11) {
        int take = run < 138 ? run : 138;
        items.push_back(RleItem{18, 7, static_cast<u16>(take - 11)});
        ++freq_cl[18];
        run -= take;
        i += take;
      }
      while (run >= 3) {
        int take = run < 10 ? run : 10;
        items.push_back(RleItem{17, 3, static_cast<u16>(take - 3)});
        ++freq_cl[17];
        run -= take;
        i += take;
      }
      while (run-- > 0) {
        items.push_back(RleItem{0, 0, 0});
        ++freq_cl[0];
        ++i;
      }
    } else {
      // Emit the length itself, then use 16 to repeat it in groups of up to 6.
      items.push_back(RleItem{cur, 0, 0});
      ++freq_cl[cur];
      ++i;
      --run;
      while (run >= 3) {
        int take = run < 6 ? run : 6;
        items.push_back(RleItem{16, 2, static_cast<u16>(take - 3)});
        ++freq_cl[16];
        run -= take;
        i += take;
      }
      while (run-- > 0) {
        items.push_back(RleItem{cur, 0, 0});
        ++freq_cl[cur];
        ++i;
      }
    }
  }
}

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

struct BitWriter {
  base::Vector<u8>* out;
  u32 bit_buffer = 0;
  int bit_count = 0;

  // Writes the low `n` bits of `value`, LSB first (inverse of BitReader::Bits).
  void WriteBits(u32 value, int n) {
    bit_buffer |= (value & ((1u << n) - 1u)) << bit_count;
    bit_count += n;
    while (bit_count >= 8) {
      out->push_back(static_cast<u8>(bit_buffer & 0xff));
      bit_buffer >>= 8;
      bit_count -= 8;
    }
  }

  // Writes a canonical Huffman code MSB-first by reversing then emitting LSB-first.
  void WriteCode(u32 code, int len) { WriteBits(ReverseBits(code, len), len); }

  void Align() {
    if (bit_count > 0) {
      out->push_back(static_cast<u8>(bit_buffer & 0xff));
      bit_buffer = 0;
      bit_count = 0;
    }
  }
};

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif

// Emits the token stream with the given lit/len and distance code tables,
// followed by the end-of-block symbol (256). Used for both the fixed and the
// dynamic code tables.
void EmitTokens(BitWriter& bw, const base::Vector<Token>& tokens, const u8* llen, const u32* lcode,
                const u8* dlen, const u32* dcode) {
  for (const Token& t : tokens) {
    if (t.dist == 0) {
      bw.WriteCode(lcode[t.sym], llen[t.sym]);
    } else {
      int li = LenSymIndex(t.sym);
      int ls = 257 + li;
      bw.WriteCode(lcode[ls], llen[ls]);
      bw.WriteBits(static_cast<u32>(t.sym - kLenBase[li]), kLenExtra[li]);
      int di = DistSymIndex(t.dist);
      bw.WriteCode(dcode[di], dlen[di]);
      bw.WriteBits(static_cast<u32>(t.dist - kDistBaseE[di]), kDistExtraE[di]);
    }
  }
  bw.WriteCode(lcode[256], llen[256]);  // end of block
}

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

}  // namespace

// GCC's -Wstringop-overflow mis-analyzes base::Vector's incremental byte appends
// under -O3 and reports bogus out-of-bounds writes; silence it around the
// byte-at-a-time bit/stream building here (clang understands the pragma too;
// MSVC does not define __GNUC__ and skips it).
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif

base::Vector<u8> ZlibDeflate(ByteSpan src) {
  constexpr int kMinMatch = 3;
  constexpr int kMaxMatch = 258;
  constexpr u32 kWindow = 32768;
  constexpr u32 kWindowMask = kWindow - 1;
  constexpr u32 kHashBits = 15;
  constexpr u32 kHashSize = 1u << kHashBits;
  constexpr u32 kHashMask = kHashSize - 1;
  constexpr int kMaxChain = 128;
  constexpr u32 kNil = 0xffffffffu;

  const u8* d = src.data();
  const size_t n = src.size();

  // --- 1. LZ77 tokenisation (done once, independent of the entropy coding) --
  // Hash-chain match finder. head[h] = most recent position with hash h;
  // chain[pos & mask] = previous position sharing that hash. Positions are u32.
  base::Vector<Token> tokens;
  u32 freq_lit[286] = {0};  // 0..255 literals, 256 EOB, 257..285 length codes.
  u32 freq_dist[30] = {0};
  u64 extra_bits = 0;  // Length + distance extra bits; identical for both codes.

  base::Vector<u32> head;
  base::Vector<u32> chain;
  head.resize(kHashSize, kNil);
  chain.resize(kWindow, kNil);

  auto hash3 = [&](size_t p) -> u32 {
    return ((static_cast<u32>(d[p]) << 10) ^ (static_cast<u32>(d[p + 1]) << 5) ^
            static_cast<u32>(d[p + 2])) &
           kHashMask;
  };
  auto insert = [&](size_t p) {
    if (p + kMinMatch <= n) {
      u32 h = hash3(p);
      chain[static_cast<u32>(p) & kWindowMask] = head[h];
      head[h] = static_cast<u32>(p);
    }
  };

  size_t pos = 0;
  while (pos < n) {
    int best_len = kMinMatch - 1;
    u32 best_dist = 0;

    if (pos + kMinMatch <= n) {
      u32 h = hash3(pos);
      u32 cand = head[h];
      size_t max_len = std::min<size_t>(kMaxMatch, n - pos);
      int chain_left = kMaxChain;
      while (cand != kNil && chain_left-- > 0) {
        u32 dist = static_cast<u32>(pos) - cand;
        if (dist == 0 || dist > kWindow) break;
        // Only bother if this candidate can beat the best (quick reject).
        if (static_cast<size_t>(best_len) < max_len && d[cand + best_len] == d[pos + best_len]) {
          size_t len = 0;
          while (len < max_len && d[cand + len] == d[pos + len]) ++len;
          if (static_cast<int>(len) > best_len) {
            best_len = static_cast<int>(len);
            best_dist = dist;
            if (len >= max_len) break;
          }
        }
        cand = chain[cand & kWindowMask];
      }
    }

    insert(pos);
    if (best_len >= kMinMatch) {
      tokens.push_back(Token{static_cast<u16>(best_len), static_cast<u16>(best_dist)});
      int li = LenSymIndex(best_len);
      ++freq_lit[257 + li];
      extra_bits += kLenExtra[li];
      int di = DistSymIndex(best_dist);
      ++freq_dist[di];
      extra_bits += kDistExtraE[di];
      size_t end = pos + best_len;
      for (++pos; pos < end; ++pos) insert(pos);
    } else {
      tokens.push_back(Token{static_cast<u16>(d[pos]), 0});
      ++freq_lit[d[pos]];
      ++pos;
    }
  }
  ++freq_lit[256];  // End-of-block symbol is always present.

  // --- 2. Fixed-Huffman code and its bit cost ------------------------------
  u8 fixed_llen[288];
  for (int i = 0; i < 144; ++i) fixed_llen[i] = 8;
  for (int i = 144; i < 256; ++i) fixed_llen[i] = 9;
  for (int i = 256; i < 280; ++i) fixed_llen[i] = 7;
  for (int i = 280; i < 288; ++i) fixed_llen[i] = 8;
  u8 fixed_dlen[30];
  for (int i = 0; i < 30; ++i) fixed_dlen[i] = 5;
  u32 fixed_lcode[288];
  u32 fixed_dcode[30];
  BuildCanonicalCodes(fixed_llen, 288, fixed_lcode);
  BuildCanonicalCodes(fixed_dlen, 30, fixed_dcode);

  u64 fixed_bits = 3;  // BFINAL + BTYPE.
  for (int s = 0; s < 286; ++s) fixed_bits += static_cast<u64>(freq_lit[s]) * fixed_llen[s];
  for (int s = 0; s < 30; ++s) fixed_bits += static_cast<u64>(freq_dist[s]) * fixed_dlen[s];
  fixed_bits += extra_bits;

  // --- 3. Dynamic-Huffman codes, the header RLE, and its bit cost ----------
  u8 dyn_llen[286];
  u8 dyn_dlen[30];
  BuildTree(freq_lit, 286, 15, dyn_llen);
  BuildTree(freq_dist, 30, 15, dyn_dlen);

  int nlit = 286;
  while (nlit > 257 && dyn_llen[nlit - 1] == 0) --nlit;
  int ndist = 30;
  while (ndist > 1 && dyn_dlen[ndist - 1] == 0) --ndist;

  u8 cl_seq[286 + 30];
  for (int i = 0; i < nlit; ++i) cl_seq[i] = dyn_llen[i];
  for (int i = 0; i < ndist; ++i) cl_seq[nlit + i] = dyn_dlen[i];
  int cl_total = nlit + ndist;

  base::Vector<RleItem> items;
  u32 freq_cl[19];
  RleCodeLengths(cl_seq, cl_total, items, freq_cl);

  u8 cl_len[19];
  BuildTree(freq_cl, 19, 7, cl_len);
  u32 cl_code[19];
  BuildCanonicalCodes(cl_len, 19, cl_code);

  int ncode = 19;
  while (ncode > 4 && cl_len[kCodeLenOrder[ncode - 1]] == 0) --ncode;

  u32 dyn_lcode[286];
  u32 dyn_dcode[30];
  BuildCanonicalCodes(dyn_llen, 286, dyn_lcode);
  BuildCanonicalCodes(dyn_dlen, 30, dyn_dcode);

  u64 dyn_bits = 3 + 5 + 5 + 4 + 3ull * static_cast<u64>(ncode);
  for (const RleItem& it : items) dyn_bits += static_cast<u64>(cl_len[it.sym]) + it.nbits;
  for (int s = 0; s < 286; ++s) dyn_bits += static_cast<u64>(freq_lit[s]) * dyn_llen[s];
  for (int s = 0; s < 30; ++s) dyn_bits += static_cast<u64>(freq_dist[s]) * dyn_dlen[s];
  dyn_bits += extra_bits;

  // --- 4. Stored size, then pick the smallest of the three -----------------
  size_t blocks = n == 0 ? 1 : (n + 0xfffe) / 0xffff;
  u64 stored_bytes = 5ull * static_cast<u64>(blocks) + static_cast<u64>(n);
  u64 fixed_bytes = (fixed_bits + 7) / 8;
  u64 dyn_bytes = (dyn_bits + 7) / 8;

  enum Mode { kFixed, kDynamic, kStored } mode = kFixed;
  u64 best = fixed_bytes;
  if (dyn_bytes < best) {
    best = dyn_bytes;
    mode = kDynamic;
  }
  if (stored_bytes < best) {
    best = stored_bytes;
    mode = kStored;
  }

  // --- 5. Emit the chosen block --------------------------------------------
  base::Vector<u8> out;
  out.reserve(2 + n + n / 2 + 5 * blocks + 64);
  out.push_back(0x78);  // CMF: 32K window, deflate.
  out.push_back(0x01);  // FLG: (CMF<<8|FLG) % 31 == 0.

  BitWriter bw{&out};
  if (mode == kStored) {
    // Byte-aligned stored blocks (out is byte-aligned after the 2-byte header).
    size_t p = 0;
    do {
      size_t chunk = std::min<size_t>(n - p, 0xffff);
      bool last = p + chunk == n;
      out.push_back(last ? 0x01 : 0x00);  // BFINAL bit, BTYPE 00 (stored).
      u16 len = static_cast<u16>(chunk);
      u16 nlen = static_cast<u16>(~len);
      out.push_back(static_cast<u8>(len));
      out.push_back(static_cast<u8>(len >> 8));
      out.push_back(static_cast<u8>(nlen));
      out.push_back(static_cast<u8>(nlen >> 8));
      for (size_t k = 0; k < chunk; ++k) out.push_back(d[p + k]);
      p += chunk;
    } while (p < n);
  } else if (mode == kDynamic) {
    bw.WriteBits(1, 1);  // BFINAL=1.
    bw.WriteBits(2, 2);  // BTYPE=10 (dynamic Huffman).
    bw.WriteBits(static_cast<u32>(nlit - 257), 5);
    bw.WriteBits(static_cast<u32>(ndist - 1), 5);
    bw.WriteBits(static_cast<u32>(ncode - 4), 4);
    for (int i = 0; i < ncode; ++i) bw.WriteBits(cl_len[kCodeLenOrder[i]], 3);
    for (const RleItem& it : items) {
      bw.WriteCode(cl_code[it.sym], cl_len[it.sym]);
      if (it.nbits) bw.WriteBits(it.val, it.nbits);
    }
    EmitTokens(bw, tokens, dyn_llen, dyn_lcode, dyn_dlen, dyn_dcode);
    bw.Align();
  } else {
    bw.WriteBits(1, 1);  // BFINAL=1.
    bw.WriteBits(1, 2);  // BTYPE=01 (fixed Huffman).
    EmitTokens(bw, tokens, fixed_llen, fixed_lcode, fixed_dlen, fixed_dcode);
    bw.Align();
  }

  // adler32 of the uncompressed data, big endian.
  u32 a = 1, b = 0;
  for (u8 c : src) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  u32 adler = (b << 16) | a;
  out.push_back(static_cast<u8>(adler >> 24));
  out.push_back(static_cast<u8>(adler >> 16));
  out.push_back(static_cast<u8>(adler >> 8));
  out.push_back(static_cast<u8>(adler));
  return out;
}

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

}  // namespace rec::bethesda
