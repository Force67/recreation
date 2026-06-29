#ifndef RECREATION_AUDIO_AUDIO_CLIP_H_
#define RECREATION_AUDIO_AUDIO_CLIP_H_

#include <memory>
#include <string_view>
#include <vector>

#include "core/types.h"

namespace rec::audio {

// The engine mixes in interleaved 32-bit float, the format SDL's audio stream
// wants and the one resampling stays cheap in. Every decoder converts its native
// sample format (PCM8/16/24/32, ADPCM nibbles, a WMA/Vorbis bitstream) into this
// one canonical representation, so the mixer never branches on source format.
struct AudioClip {
  u32 channels = 0;
  u32 sample_rate = 0;
  std::vector<float> samples;  // interleaved, channels per frame

  u64 frames() const { return channels ? samples.size() / channels : 0; }
  bool valid() const { return channels > 0 && sample_rate > 0 && !samples.empty(); }
};

// A pull-model source of decoded audio. Short sounds decode fully into an
// AudioClip up front (see DecodeClip); long ones (music, ambience loops) keep a
// live Decoder and stream under the mixer, so a minutes-long track never sits
// resident as raw PCM. Read/Rewind are only ever called from the mixer thread.
class Decoder {
 public:
  virtual ~Decoder() = default;

  virtual u32 channels() const = 0;
  virtual u32 sample_rate() const = 0;
  // Total frames when known, 0 when the length is not cheaply available.
  virtual u64 frame_count() const = 0;

  // Decodes up to `frames` frames into `out` (channels()*frames floats) and
  // returns how many frames were actually produced; 0 means end of stream.
  virtual u32 Read(float* out, u32 frames) = 0;

  // Restarts decoding from the first frame, for looping streams. False when the
  // source cannot seek (then the mixer drops the voice at end of stream).
  virtual bool Rewind() = 0;
};

// Opens a streaming decoder for `bytes`, dispatched by the file extension with a
// magic-number fallback (a .fuz is really a wrapped xWMA, a mod's loose file may
// lie about its extension). `bytes` is copied into the decoder, so the caller's
// buffer need not outlive it. Null when no decoder handles the data.
std::unique_ptr<Decoder> OpenDecoder(ByteSpan bytes, std::string_view extension);

// Decodes `bytes` completely into an AudioClip. Returns an invalid clip (see
// AudioClip::valid) when the format is unsupported or the data is malformed.
AudioClip DecodeClip(ByteSpan bytes, std::string_view extension);

// Wraps an already-decoded clip in a streaming Decoder (a cursor over its
// samples), so a cached one-shot can be handed to the mixer like any other
// source. The clip is moved in; play a copy to fire the same sound twice.
std::unique_ptr<Decoder> MakeClipDecoder(AudioClip clip);

}  // namespace rec::audio

#endif  // RECREATION_AUDIO_AUDIO_CLIP_H_
