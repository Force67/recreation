#ifndef RECREATION_AUDIO_MIXER_H_
#define RECREATION_AUDIO_MIXER_H_

#include <memory>
#include <mutex>
#include <vector>

#include "audio/audio_clip.h"
#include "audio/spatial.h"
#include "core/math.h"
#include "core/types.h"

namespace rec::audio {

// How a voice should be placed and looped when it starts.
struct PlayParams {
  f32 gain = 1.0f;
  bool loop = false;
  bool positional = false;  // false = a 2D UI/menu/global sound, equal in both ears
  Vec3 position{};
  Attenuation atten{};
  // Linear fade-in over this many seconds (0 = start at full gain). Ambient beds
  // fade in so a region change does not snap a loop on.
  f32 fade_in = 0.0f;
};

// The software mixer. The engine thread submits commands (start/stop a voice,
// move it, move the listener); the audio device thread drains them and renders
// interleaved stereo float at the device sample rate. The command queue is the
// only shared state, guarded by a mutex held for the few microseconds it takes to
// splice vectors, never across decoding or mixing.
class Mixer {
 public:
  // `output_rate` is the device's mix rate; every voice resamples to it.
  void Configure(u32 output_rate) { output_rate_ = output_rate; }
  u32 output_rate() const { return output_rate_; }

  // --- engine thread ---------------------------------------------------------
  // Starts `decoder` as a new voice and returns its id (0 if decoder is null).
  // The returned id stays valid for Stop/SetVoice* until the voice finishes.
  u32 Play(std::unique_ptr<Decoder> decoder, const PlayParams& params);
  void Stop(u32 voice, f32 fade_out = 0.05f);
  void StopAll();
  void SetVoiceGain(u32 voice, f32 gain);
  void SetVoicePosition(u32 voice, const Vec3& position);
  void SetListener(const Listener& listener);
  void SetMasterGain(f32 gain);  // 0 mutes; applied on the device thread

  // --- device thread ---------------------------------------------------------
  // Renders `frames` interleaved stereo frames into `out` (2*frames floats),
  // overwriting whatever was there.
  void MixInto(float* out, u32 frames);

 private:
  struct Voice {
    u32 id = 0;
    std::unique_ptr<Decoder> decoder;
    PlayParams params;
    // Decoded-but-not-yet-resampled source frames, consumed from `head`.
    std::vector<float> src;
    size_t head = 0;        // first unconsumed source frame
    f64 frac = 0.0;         // fractional position between head and head+1
    u32 src_channels = 1;
    bool ended = false;     // decoder returned EOS and is not looping
    bool dead = false;      // remove now (a fade-out completed)
    // Gain envelope, ramped on the device thread to avoid zipper noise.
    f32 gain = 1.0f;        // target user gain
    f32 env = 0.0f;         // current envelope level (starts at fade-in floor)
    f32 env_rate = 0.0f;    // per-frame envelope step toward 1 (fade-in) ...
    bool stopping = false;  // ... or toward 0 (fade-out), then removed
    StereoGains current{0, 0};  // smoothed channel gains
  };

  // Drains the command queue into the live voice list (device thread).
  void ApplyCommands();
  // Ensures `voice.src` holds at least `frames` source frames past `head`,
  // decoding and looping as needed. Returns false at a hard end of stream.
  bool FillSource(Voice& voice, size_t frames);

  enum class CmdType { kPlay, kStop, kStopAll, kGain, kPosition, kListener, kMaster };
  struct Command {
    CmdType type;
    u32 voice = 0;
    f32 value = 0.0f;
    Vec3 position{};
    std::unique_ptr<Decoder> decoder;
    PlayParams params;
    Listener listener;
  };

  u32 output_rate_ = 48000;
  std::mutex mutex_;
  std::vector<Command> pending_;
  u32 next_id_ = 1;

  // Device-thread-only state below.
  std::vector<Voice> voices_;
  Listener listener_;
  f32 master_ = 1.0f;
};

}  // namespace rec::audio

#endif  // RECREATION_AUDIO_MIXER_H_
