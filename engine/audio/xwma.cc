#include "audio/xwma.h"

#include <cstring>
#include <vector>

#include "audio/ffmpeg_codec.h"
#include "audio/wav.h"
#include "core/log.h"

namespace rec::audio {
namespace {

u32 ReadU32LE(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }
u16 ReadU16LE(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }

// A chunk found inside a RIFF container.
struct Chunk {
  const u8* data = nullptr;
  u32 size = 0;
};

// Walks the chunks of a RIFF body (the bytes after "RIFF"<size><form>) and
// returns the first chunk with the given four-cc, or an empty chunk.
Chunk FindChunk(ByteSpan riff, u32 tag) {
  if (riff.size() < 12) return {};
  size_t off = 12;  // skip RIFF + size + form id
  while (off + 8 <= riff.size()) {
    const u8* p = riff.data() + off;
    const u32 id = FourCc(static_cast<char>(p[0]), static_cast<char>(p[1]),
                          static_cast<char>(p[2]), static_cast<char>(p[3]));
    u32 size = ReadU32LE(p + 4);
    const u8* body = p + 8;
    if (body + size > riff.data() + riff.size()) size = static_cast<u32>(riff.data() + riff.size() - body);
    if (id == tag) return {body, size};
    off += 8 + size + (size & 1);  // chunks are word-aligned
  }
  return {};
}

// Builds a standard RIFF/WAVE file around a raw fmt chunk and PCM data, so a Wwise
// PCM payload can be decoded by the native WAV path instead of a codec backend.
std::vector<u8> WrapAsWave(const Chunk& fmt, const Chunk& data) {
  std::vector<u8> w;
  auto tag = [&](const char* t) {
    for (int i = 0; i < 4; ++i) w.push_back(static_cast<u8>(t[i]));
  };
  auto u32v = [&](u32 v) {
    for (int i = 0; i < 4; ++i) w.push_back((v >> (8 * i)) & 0xFF);
  };
  const u32 riff_size = 4 + (8 + fmt.size + (fmt.size & 1)) + (8 + data.size);
  tag("RIFF");
  u32v(riff_size);
  tag("WAVE");
  tag("fmt ");
  u32v(fmt.size);
  w.insert(w.end(), fmt.data, fmt.data + fmt.size);
  if (fmt.size & 1) w.push_back(0);
  tag("data");
  u32v(data.size);
  w.insert(w.end(), data.data, data.data + data.size);
  return w;
}

// Wwise PCM and IEEE float can be decoded natively; everything else (Wwise Vorbis,
// xWMA, WMA) needs a codec backend.
std::unique_ptr<Decoder> OpenWem(ByteSpan bytes) {
  Chunk fmt = FindChunk(bytes, FourCc('f', 'm', 't', ' '));
  Chunk data = FindChunk(bytes, FourCc('d', 'a', 't', 'a'));
  if (fmt.data && fmt.size >= 16 && data.data) {
    const u16 codec = ReadU16LE(fmt.data);
    constexpr u16 kPcm = 0x0001, kFloat = 0x0003, kExtensible = 0xFFFE;
    u16 real = codec;
    if (codec == kExtensible && fmt.size >= 26) real = ReadU16LE(fmt.data + 24);
    if (real == kPcm || real == kFloat) {
      std::vector<u8> wave = WrapAsWave(fmt, data);
      AudioClip clip;
      if (DecodeWav(ByteSpan{wave.data(), wave.size()}, &clip)) return MakeClipDecoder(std::move(clip));
    }
  }
  // Compressed Wwise media (typically Wwise Vorbis): best-effort through FFmpeg.
  return OpenFfmpegDecoder(bytes);
}

// FUZ = "FUZE" magic, a version, a LIP (lipsync) block, then an embedded xWMA
// RIFF. Strip the header and lip block and decode the audio that follows.
std::unique_ptr<Decoder> OpenFuz(ByteSpan bytes) {
  if (bytes.size() < 12) return nullptr;
  const u32 lip_size = ReadU32LE(bytes.data() + 8);
  size_t audio_off = 12 + lip_size;
  if (audio_off + 12 > bytes.size()) {
    // Some tools omit/!align the lip size; fall back to scanning for the RIFF.
    audio_off = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())
                    .find("RIFF", 12);
    if (audio_off == std::string_view::npos) return nullptr;
  }
  ByteSpan audio = bytes.subspan(audio_off);
  return OpenFfmpegDecoder(audio);
}

}  // namespace

std::unique_ptr<Decoder> OpenCompressed(ByteSpan bytes, CompressedKind kind) {
  switch (kind) {
    case CompressedKind::kXwma:
      return OpenFfmpegDecoder(bytes);
    case CompressedKind::kFuz:
      return OpenFuz(bytes);
    case CompressedKind::kWem:
      return OpenWem(bytes);
  }
  return nullptr;
}

}  // namespace rec::audio
