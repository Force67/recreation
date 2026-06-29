#ifndef RECREATION_AUDIO_AUDIO_DEVICE_H_
#define RECREATION_AUDIO_AUDIO_DEVICE_H_

#include <vector>

#include "core/types.h"

namespace rec::audio {

class Mixer;

// The output sink: opens the default playback device through SDL's audio stream
// and pulls interleaved stereo float from the mixer on SDL's audio thread. When
// SDL audio is not compiled in (Android's native backend, a headless server) Open
// is a no-op that reports inactive, and the rest of the audio system runs silent.
class AudioDevice {
 public:
  ~AudioDevice();

  // The fixed rate the mixer renders at; SDL converts to the device's native
  // format. Stereo, 32-bit float.
  static constexpr u32 kMixRate = 48000;

  // Binds `mixer` and starts playback. False when no device is available.
  bool Open(Mixer* mixer);
  void Close();
  bool active() const { return active_; }

  // Called from SDL's audio thread to render `additional_bytes` of stereo float
  // into the given SDL_AudioStream (passed as void* so the header stays SDL-free).
  // Public only so the file-local SDL callback can reach it; not for general use.
  void RenderInto(void* sdl_stream, int additional_bytes);

 private:
  void* stream_ = nullptr;  // SDL_AudioStream*, opaque so the header stays SDL-free
  Mixer* mixer_ = nullptr;
  std::vector<float> scratch_;  // audio-thread-only render buffer
  bool active_ = false;
};

}  // namespace rec::audio

#endif  // RECREATION_AUDIO_AUDIO_DEVICE_H_
