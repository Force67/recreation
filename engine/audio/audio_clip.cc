#include "audio/audio_clip.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "audio/wav.h"
#include "core/log.h"

namespace rec::audio {
namespace {

std::string LowerExt(std::string_view extension) {
  std::string e(extension);
  if (!e.empty() && e.front() == '.') e.erase(e.begin());
  std::transform(e.begin(), e.end(), e.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return e;
}

// The four-byte tag at `offset`, or 0 when the buffer is too short. Used to
// recognise a container whose extension lies (a .fuz is a wrapped xWMA, a mod's
// loose file may be misnamed).
u32 TagAt(ByteSpan bytes, size_t offset) {
  if (bytes.size() < offset + 4) return 0;
  const u8* p = bytes.data() + offset;
  return FourCc(static_cast<char>(p[0]), static_cast<char>(p[1]), static_cast<char>(p[2]),
                static_cast<char>(p[3]));
}

// Recognised container kinds, resolved from the extension with a magic-number
// fallback so the dispatch is robust to misnamed files.
enum class Container { kUnknown, kWav, kXwma, kFuz, kWem };

Container Classify(ByteSpan bytes, std::string_view extension) {
  const std::string ext = LowerExt(extension);
  if (ext == "wav") return Container::kWav;
  if (ext == "xwm") return Container::kXwma;
  if (ext == "fuz") return Container::kFuz;
  if (ext == "wem") return Container::kWem;

  // Sniff the header when the extension is missing or wrong.
  if (TagAt(bytes, 0) == FourCc('F', 'U', 'Z', 'E')) return Container::kFuz;
  if (TagAt(bytes, 0) == FourCc('R', 'I', 'F', 'F')) {
    u32 form = TagAt(bytes, 8);
    if (form == FourCc('W', 'A', 'V', 'E')) return Container::kWav;
    if (form == FourCc('X', 'W', 'M', 'A')) return Container::kXwma;
    return Container::kWem;  // Wwise media is a RIFF with a numeric form id
  }
  return Container::kUnknown;
}

// Streams an already fully-decoded clip: short sounds (most sfx, ambience loops)
// decode up front and play back through this thin cursor, so the mixer only ever
// talks to the Decoder interface whatever the source was.
class ClipDecoder final : public Decoder {
 public:
  explicit ClipDecoder(AudioClip clip) : clip_(std::move(clip)) {}

  u32 channels() const override { return clip_.channels; }
  u32 sample_rate() const override { return clip_.sample_rate; }
  u64 frame_count() const override { return clip_.frames(); }

  u32 Read(float* out, u32 frames) override {
    const u64 remaining = clip_.frames() - cursor_;
    const u32 n = static_cast<u32>(std::min<u64>(frames, remaining));
    if (n > 0) {
      std::memcpy(out, clip_.samples.data() + cursor_ * clip_.channels,
                  static_cast<size_t>(n) * clip_.channels * sizeof(float));
      cursor_ += n;
    }
    return n;
  }

  bool Rewind() override {
    cursor_ = 0;
    return true;
  }

 private:
  AudioClip clip_;
  u64 cursor_ = 0;
};

}  // namespace

AudioClip DecodeClip(ByteSpan bytes, std::string_view extension) {
  AudioClip clip;
  switch (Classify(bytes, extension)) {
    case Container::kWav:
      DecodeWav(bytes, &clip);
      break;
    case Container::kXwma:
    case Container::kFuz:
    case Container::kWem:
      // Compressed containers stream through the codec backend (see xwma.cc),
      // which DecodeClip drains in OpenDecoder; nothing to do up front here.
      break;
    case Container::kUnknown:
      break;
  }
  return clip;
}

std::unique_ptr<Decoder> MakeClipDecoder(AudioClip clip) {
  if (!clip.valid()) return nullptr;
  return std::make_unique<ClipDecoder>(std::move(clip));
}

std::unique_ptr<Decoder> OpenDecoder(ByteSpan bytes, std::string_view extension) {
  return MakeClipDecoder(DecodeClip(bytes, extension));
}

}  // namespace rec::audio
