#include "audio/wav.h"

#include <cstring>

#include "core/log.h"
#include "core/types.h"

// Native RIFF/WAVE decoder. WAV is the one game audio container with no licensed
// codec behind it, so it is decoded here from first principles rather than handed
// to the optional FFmpeg backend: uncompressed sound effects and the ADPCM ones
// play with zero external dependencies, on every platform, in CI.
namespace rec::audio {
namespace {

// Little-endian byte cursor over the file. Every read is bounds-checked; on
// overrun it latches `ok = false` and returns zero, so a truncated file falls
// out of the parse instead of reading past the buffer.
struct Reader {
  const u8* p = nullptr;
  size_t left = 0;
  bool ok = true;

  u8 U8() {
    if (left < 1) return Fail();
    --left;
    return *p++;
  }
  u16 U16() {
    u16 lo = U8(), hi = U8();
    return static_cast<u16>(lo | (hi << 8));
  }
  u32 U32() {
    u32 a = U8(), b = U8(), c = U8(), d = U8();
    return a | (b << 8) | (c << 16) | (d << 24);
  }
  void Skip(size_t n) {
    if (left < n) {
      left = 0;
      ok = false;
      return;
    }
    p += n;
    left -= n;
  }
  u8 Fail() {
    ok = false;
    left = 0;
    return 0;
  }
};

constexpr u16 kFormatPcm = 0x0001;
constexpr u16 kFormatMsAdpcm = 0x0002;
constexpr u16 kFormatIeeeFloat = 0x0003;
constexpr u16 kFormatImaAdpcm = 0x0011;
constexpr u16 kFormatExtensible = 0xFFFE;

// The parsed `fmt ` chunk, normalized so EXTENSIBLE collapses to its real tag.
struct Format {
  u16 tag = 0;
  u16 channels = 0;
  u32 sample_rate = 0;
  u16 block_align = 0;
  u16 bits = 0;
  // ADPCM samples-per-block, read from the format extension (0 for PCM/float).
  u16 samples_per_block = 0;
  // MS-ADPCM predictor coefficients (pairs), defaulted to the standard 7 the
  // encoder uses unless the file ships its own table.
  std::vector<i16> coef1;
  std::vector<i16> coef2;
};

float SampleU8(u8 v) { return (static_cast<float>(v) - 128.0f) / 128.0f; }
float SampleI16(i16 v) { return static_cast<float>(v) / 32768.0f; }

void PushI16(AudioClip* out, i32 v) {
  if (v < -32768) v = -32768;
  if (v > 32767) v = 32767;
  out->samples.push_back(SampleI16(static_cast<i16>(v)));
}

// --- integer PCM / IEEE float -------------------------------------------------

bool DecodePcm(Reader& r, const Format& fmt, AudioClip* out) {
  const size_t bytes_per_sample = fmt.bits / 8u;
  if (bytes_per_sample == 0) return false;
  const size_t total = r.left / bytes_per_sample;
  out->samples.reserve(total);
  for (size_t i = 0; i < total && r.ok; ++i) {
    if (fmt.tag == kFormatIeeeFloat && fmt.bits == 32) {
      u32 bits = r.U32();
      float f;
      std::memcpy(&f, &bits, sizeof(f));
      out->samples.push_back(f);
    } else if (fmt.bits == 8) {
      out->samples.push_back(SampleU8(r.U8()));
    } else if (fmt.bits == 16) {
      out->samples.push_back(SampleI16(static_cast<i16>(r.U16())));
    } else if (fmt.bits == 24) {
      u32 a = r.U8(), b = r.U8(), c = r.U8();
      i32 v = static_cast<i32>(a | (b << 8) | (c << 16));
      if (v & 0x800000) v |= ~0xFFFFFF;  // sign-extend 24 -> 32
      out->samples.push_back(static_cast<float>(v) / 8388608.0f);
    } else if (fmt.bits == 32) {
      i32 v = static_cast<i32>(r.U32());
      out->samples.push_back(static_cast<float>(v) / 2147483648.0f);
    } else {
      return false;
    }
  }
  return out->valid();
}

// --- IMA / DVI ADPCM (0x0011) -------------------------------------------------

const int kImaIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
const int kImaStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,   23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,   80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,  279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,  963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024, 3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

int ImaNibble(u8 nibble, int& predictor, int& index) {
  int step = kImaStepTable[index];
  int diff = step >> 3;
  if (nibble & 1) diff += step >> 2;
  if (nibble & 2) diff += step >> 1;
  if (nibble & 4) diff += step;
  if (nibble & 8) diff = -diff;
  predictor += diff;
  if (predictor > 32767) predictor = 32767;
  if (predictor < -32768) predictor = -32768;
  index += kImaIndexTable[nibble];
  if (index < 0) index = 0;
  if (index > 88) index = 88;
  return predictor;
}

bool DecodeImaAdpcm(Reader& r, const Format& fmt, AudioClip* out) {
  const u32 ch = fmt.channels;
  if (ch == 0 || fmt.block_align < 4u * ch) return false;
  std::vector<u8> block(fmt.block_align);
  std::vector<int> predictor(ch), index(ch);
  while (r.left >= fmt.block_align && r.ok) {
    for (u16 i = 0; i < fmt.block_align; ++i) block[i] = r.U8();
    const u8* b = block.data();
    // Per-channel block header: predictor (i16), step index (u8), reserved (u8).
    for (u32 c = 0; c < ch; ++c) {
      predictor[c] = static_cast<i16>(b[0] | (b[1] << 8));
      index[c] = b[2];
      if (index[c] > 88) index[c] = 88;
      PushI16(out, predictor[c]);
      b += 4;
    }
    // Remaining bytes are 4-byte words, interleaved per channel, 8 nibbles each.
    // Decode each channel's word into a staging row, then emit the 8 frames
    // interleaved so the output stays channel-major per frame.
    const u8* end = block.data() + fmt.block_align;
    std::vector<int> staged(static_cast<size_t>(ch) * 8);
    while (b + 4u * ch <= end) {
      for (u32 c = 0; c < ch; ++c) {
        for (int n = 0; n < 4; ++n) {
          u8 byte = b[c * 4 + n];  // low nibble is the earlier sample
          staged[c * 8 + n * 2 + 0] = ImaNibble(byte & 0x0F, predictor[c], index[c]);
          staged[c * 8 + n * 2 + 1] = ImaNibble(byte >> 4, predictor[c], index[c]);
        }
      }
      for (int s = 0; s < 8; ++s)
        for (u32 c = 0; c < ch; ++c) PushI16(out, staged[c * 8 + s]);
      b += 4u * ch;
    }
  }
  return out->valid();
}

// --- Microsoft ADPCM (0x0002) -------------------------------------------------

const int kMsAdaptTable[16] = {230, 230, 230, 230, 307, 409, 512, 614,
                               768, 614, 512, 409, 307, 230, 230, 230};
const i16 kMsCoef1[7] = {256, 512, 0, 192, 240, 460, 392};
const i16 kMsCoef2[7] = {0, -256, 0, 64, 0, -208, -232};

int MsNibble(u8 nibble, int& samp1, int& samp2, int& delta, i16 coef1, i16 coef2) {
  int signed_nibble = (nibble & 0x08) ? (nibble - 16) : nibble;
  int predict = (samp1 * coef1 + samp2 * coef2) >> 8;
  int value = predict + signed_nibble * delta;
  if (value > 32767) value = 32767;
  if (value < -32768) value = -32768;
  samp2 = samp1;
  samp1 = value;
  delta = (kMsAdaptTable[nibble] * delta) >> 8;
  if (delta < 16) delta = 16;
  return value;
}

bool DecodeMsAdpcm(Reader& r, const Format& fmt, AudioClip* out) {
  const u32 ch = fmt.channels;
  if (ch == 0 || ch > 2 || fmt.block_align < 7u * ch) return false;
  std::vector<u8> block(fmt.block_align);
  while (r.left >= fmt.block_align && r.ok) {
    for (u16 i = 0; i < fmt.block_align; ++i) block[i] = r.U8();
    const u8* b = block.data();
    const u8* end = block.data() + fmt.block_align;

    int predictor[2] = {0, 0}, delta[2] = {0, 0}, samp1[2] = {0, 0}, samp2[2] = {0, 0};
    i16 c1[2] = {0, 0}, c2[2] = {0, 0};
    for (u32 c = 0; c < ch; ++c) {
      predictor[c] = *b++;
      if (predictor[c] >= static_cast<int>(fmt.coef1.size())) predictor[c] = 0;
      c1[c] = fmt.coef1[predictor[c]];
      c2[c] = fmt.coef2[predictor[c]];
    }
    auto read_i16 = [&]() {
      i16 v = static_cast<i16>(b[0] | (b[1] << 8));
      b += 2;
      return v;
    };
    for (u32 c = 0; c < ch; ++c) delta[c] = read_i16();
    for (u32 c = 0; c < ch; ++c) samp1[c] = read_i16();
    for (u32 c = 0; c < ch; ++c) samp2[c] = read_i16();

    // The block opens with samp2 then samp1 for each channel (in playback order).
    for (u32 c = 0; c < ch; ++c) PushI16(out, samp2[c]);
    for (u32 c = 0; c < ch; ++c) PushI16(out, samp1[c]);

    // Remaining bytes hold two nibbles each, alternating channels for stereo.
    u32 c = 0;
    while (b < end && r.ok) {
      u8 byte = *b++;
      int hi = MsNibble(byte >> 4, samp1[c], samp2[c], delta[c], c1[c], c2[c]);
      PushI16(out, hi);
      c = (c + 1) % ch;
      int lo = MsNibble(byte & 0x0F, samp1[c], samp2[c], delta[c], c1[c], c2[c]);
      PushI16(out, lo);
      c = (c + 1) % ch;
    }
  }
  return out->valid();
}

}  // namespace

bool DecodeWav(ByteSpan bytes, AudioClip* out) {
  out->samples.clear();
  out->channels = 0;
  out->sample_rate = 0;

  Reader r{bytes.data(), bytes.size()};
  if (r.U32() != FourCc('R', 'I', 'F', 'F')) return false;
  r.U32();  // RIFF chunk size (unreliable, ignored)
  if (r.U32() != FourCc('W', 'A', 'V', 'E')) return false;

  Format fmt;
  bool have_fmt = false;
  Reader data{};  // a view over the 'data' chunk, captured then decoded last

  while (r.left >= 8 && r.ok) {
    u32 id = r.U32();
    u32 size = r.U32();
    if (size > r.left) size = static_cast<u32>(r.left);
    const u8* chunk = r.p;

    if (id == FourCc('f', 'm', 't', ' ')) {
      Reader f{chunk, size};
      fmt.tag = f.U16();
      fmt.channels = f.U16();
      fmt.sample_rate = f.U32();
      f.U32();  // avg bytes/sec
      fmt.block_align = f.U16();
      fmt.bits = f.U16();
      if (size >= 18) {
        u16 cb = f.U16();  // extension size
        if (fmt.tag == kFormatExtensible && cb >= 22) {
          f.U16();  // valid bits per sample
          f.U32();  // channel mask
          fmt.tag = f.U16();  // first 2 bytes of the subformat GUID = real tag
        } else if (fmt.tag == kFormatMsAdpcm) {
          fmt.samples_per_block = f.U16();
          u16 num_coef = f.U16();
          for (u16 i = 0; i < num_coef && f.ok; ++i) {
            fmt.coef1.push_back(static_cast<i16>(f.U16()));
            fmt.coef2.push_back(static_cast<i16>(f.U16()));
          }
        } else if (fmt.tag == kFormatImaAdpcm) {
          fmt.samples_per_block = f.U16();
        }
      }
      if (fmt.coef1.empty()) {
        fmt.coef1.assign(kMsCoef1, kMsCoef1 + 7);
        fmt.coef2.assign(kMsCoef2, kMsCoef2 + 7);
      }
      have_fmt = true;
    } else if (id == FourCc('d', 'a', 't', 'a')) {
      data = Reader{chunk, size};
    }

    r.Skip(size);
    if (size & 1) r.Skip(1);  // chunks are word-aligned
  }

  if (!have_fmt || data.p == nullptr || fmt.channels == 0 || fmt.sample_rate == 0) return false;
  out->channels = fmt.channels;
  out->sample_rate = fmt.sample_rate;

  bool ok = false;
  switch (fmt.tag) {
    case kFormatPcm:
    case kFormatIeeeFloat:
      ok = DecodePcm(data, fmt, out);
      break;
    case kFormatImaAdpcm:
      ok = DecodeImaAdpcm(data, fmt, out);
      break;
    case kFormatMsAdpcm:
      ok = DecodeMsAdpcm(data, fmt, out);
      break;
    default:
      REC_WARN("wav: unsupported format tag 0x{:04x}", fmt.tag);
      break;
  }
  if (!ok) {
    out->samples.clear();
    out->channels = 0;
    out->sample_rate = 0;
  }
  return ok;
}

}  // namespace rec::audio
