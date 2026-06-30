#ifndef RECREATION_AUDIO_AUDIO_SYSTEM_H_
#define RECREATION_AUDIO_AUDIO_SYSTEM_H_

#include <string>
#include <string_view>
#include <unordered_map>

#include "audio/audio_clip.h"
#include "audio/audio_device.h"
#include "audio/mixer.h"
#include "audio/spatial.h"
#include "core/math.h"
#include "core/types.h"

namespace rec::asset {
class Vfs;
}

namespace rec::audio {

// The engine-facing audio facade. Owns the output device, the mixer and a small
// decoded-clip cache, and resolves sound paths through the game Vfs (loose files
// + BSA/BA2), so a caller just names an asset path. One instance lives on the
// engine; positional helpers take engine-space (Y-up, metres) coordinates.
//
// Suppression: REC_AUDIO_MUTE (base::Option "audio.mute") opens no device and
// makes every play a no-op; REC_AUDIO_VOLUME ("audio.volume", 0..1) sets the
// master level. Both are read once at Initialize.
class AudioSystem {
 public:
  // `vfs` supplies sound bytes; may be null (then only absolute callers that hand
  // over bytes directly work, and path lookups fail quietly). Returns false when
  // audio is suppressed or no device opened, but the object stays usable (silent).
  bool Initialize(asset::Vfs* vfs);
  void Shutdown();

  bool active() const { return device_.active(); }
  bool muted() const { return muted_; }

  // Per frame: update the listener pose so positional voices pan/attenuate.
  void SetListener(const Vec3& position, const Vec3& forward, const Vec3& up);

  // Fire-and-forget 2D sound (UI clicks, non-diegetic cues), equal in both ears.
  u32 PlayUi(std::string_view path, f32 gain = 1.0f);
  // Positional one-shot at a world position; `params.position` is overwritten.
  u32 PlayAt(std::string_view path, const Vec3& position, PlayParams params = {});
  // Looping voice (ambient bed, music). Streams compressed sources. Returns the
  // voice id for a later Stop / SetVoicePosition; 0 on failure.
  u32 PlayLoop(std::string_view path, PlayParams params);

  void Stop(u32 voice, f32 fade = 0.12f);
  void StopAll();
  void SetVoicePosition(u32 voice, const Vec3& position);
  void SetVoiceGain(u32 voice, f32 gain);

  void SetMasterVolume(f32 volume);
  f32 master_volume() const { return master_; }

  // Whether `path` exists in the mounted Vfs (loose files + archives). Lets a
  // caller pick a sound it can actually load before committing to play it.
  bool HasAsset(std::string_view path) const;

  Mixer& mixer() { return mixer_; }

 private:
  // Decodes (and caches) a short sound fully. Returns null on failure; failures
  // cache as an empty clip so a missing/unsupported file is only probed once.
  const AudioClip* GetClip(std::string_view path);
  // Opens a streaming decoder for a long sound (caller owns it). Null on failure.
  std::unique_ptr<Decoder> OpenStream(std::string_view path);
  // Reads `path` from the Vfs into `out`; false when absent. Held as a member so
  // the byte buffer outlives the ByteSpan handed to the decoder.
  bool ReadAsset(std::string_view path, std::vector<u8>* out);

  asset::Vfs* vfs_ = nullptr;
  Mixer mixer_;
  AudioDevice device_;
  std::unordered_map<std::string, AudioClip> clip_cache_;
  bool muted_ = false;
  f32 master_ = 1.0f;
};

}  // namespace rec::audio

#endif  // RECREATION_AUDIO_AUDIO_SYSTEM_H_
