#include "audio/audio_device.h"

#include "audio/mixer.h"
#include "core/log.h"

#if defined(RECREATION_HAS_SDL3_AUDIO)
#include <SDL3/SDL.h>
#endif

namespace rec::audio {

#if defined(RECREATION_HAS_SDL3_AUDIO)

namespace {

// SDL's audio thread asks for `additional` more bytes. Render exactly that many
// stereo float frames from the mixer and hand them back. SDL owns the conversion
// to the device's native format and rate, so the mixer always speaks f32/48k.
void SDLCALL FeedStream(void* userdata, SDL_AudioStream* stream, int additional, int /*total*/) {
  if (additional <= 0) return;
  static_cast<AudioDevice*>(userdata)->RenderInto(stream, additional);
}

}  // namespace

void AudioDevice::RenderInto(void* sdl_stream, int additional_bytes) {
  auto* stream = static_cast<SDL_AudioStream*>(sdl_stream);
  const int frames = additional_bytes / static_cast<int>(sizeof(float) * 2);
  if (frames <= 0 || !mixer_) return;
  if (scratch_.size() < static_cast<size_t>(frames) * 2)
    scratch_.resize(static_cast<size_t>(frames) * 2);
  mixer_->MixInto(scratch_.data(), static_cast<u32>(frames));
  SDL_PutAudioStreamData(stream, scratch_.data(), frames * static_cast<int>(sizeof(float) * 2));
}

bool AudioDevice::Open(Mixer* mixer) {
  if (!mixer) return false;
  mixer_ = mixer;
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    REC_WARN("audio: SDL audio init failed: {}", SDL_GetError());
    return false;
  }
  SDL_AudioSpec spec;
  spec.format = SDL_AUDIO_F32;
  spec.channels = 2;
  spec.freq = static_cast<int>(kMixRate);
  mixer_->Configure(kMixRate);

  SDL_AudioStream* stream =
      SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, FeedStream, this);
  if (!stream) {
    REC_WARN("audio: no playback device: {}", SDL_GetError());
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return false;
  }
  stream_ = stream;
  SDL_ResumeAudioStreamDevice(stream);
  active_ = true;
  REC_INFO("audio: device open, {} Hz stereo f32", kMixRate);
  return true;
}

void AudioDevice::Close() {
  if (stream_) {
    SDL_DestroyAudioStream(static_cast<SDL_AudioStream*>(stream_));
    stream_ = nullptr;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
  }
  active_ = false;
}

#else  // no SDL audio: a silent device so the rest of the system is unconditional.

bool AudioDevice::Open(Mixer* mixer) {
  mixer_ = mixer;
  if (mixer_) mixer_->Configure(kMixRate);
  REC_INFO("audio: built without SDL audio, running silent");
  return false;
}

void AudioDevice::Close() { active_ = false; }

void AudioDevice::RenderInto(void*, int) {}

#endif

AudioDevice::~AudioDevice() { Close(); }

}  // namespace rec::audio
