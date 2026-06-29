#ifndef RECREATION_AUDIO_AMBIENT_H_
#define RECREATION_AUDIO_AMBIENT_H_

#include <string>
#include <unordered_map>
#include <vector>

#include "audio/sound_catalog.h"
#include "bethesda/form_id.h"
#include "core/types.h"

namespace rec::bethesda {
class RecordStore;
}

namespace rec::audio {

class AudioSystem;

// REGN region -> its authored ambient sounds. A Skyrim region carries a weighted
// list of looping ambiences (the wind on the tundra, the gulls at the coast); we
// parse the sound entries out of the region records and, given a region, return
// the sound forms it should play. Pure data, like weather's RegionWeather.
class RegionAmbience {
 public:
  // Parses every REGN's sound section into the region -> sound-forms map.
  void Build(const bethesda::RecordStore& records);

  // The ambient sound forms for a region (resolve them through a SoundCatalog),
  // or an empty list when the region has none.
  const std::vector<bethesda::GlobalFormId>& SoundsFor(u64 region) const;

  bool empty() const { return region_sounds_.empty(); }
  size_t size() const { return region_sounds_.size(); }

 private:
  std::unordered_map<u64, std::vector<bethesda::GlobalFormId>> region_sounds_;
  std::vector<bethesda::GlobalFormId> empty_;
};

// Where the player is, distilled to what picks an ambient bed.
struct AmbientContext {
  bool interior = false;
  u64 region = 0;  // active REGN form (0 when in no region / interior)
};

// Decides what to do when the chosen bed changes: a different bed stops the old
// and starts the new (a cross-fade), the same bed is left alone, and an empty
// target just stops. Pure, so the transition policy is unit-tested without audio.
struct AmbientDecision {
  bool stop_current = false;
  bool start_target = false;
};
AmbientDecision DecideAmbient(const std::string& current, const std::string& target);

// Drives the looping ambient bed from the player's area. Each frame it is handed
// the current context, resolves it to a sound file, and cross-fades the bed when
// the file changes. Silent (and harmless) until configured with a live audio
// system, and whenever a context resolves to no playable file.
class AmbientDirector {
 public:
  void Configure(AudioSystem* audio, const SoundCatalog* catalog, const RegionAmbience* regions) {
    audio_ = audio;
    catalog_ = catalog;
    regions_ = regions;
  }

  // Resolves `context` to a bed and applies the cross-fade if it changed.
  void Update(const AmbientContext& context);
  // Fades out the active bed (leaving an interior, unloading a game).
  void Stop();

  const std::string& current_bed() const { return current_path_; }

 private:
  // The asset path the context should play, or empty for silence.
  std::string Resolve(const AmbientContext& context) const;

  AudioSystem* audio_ = nullptr;
  const SoundCatalog* catalog_ = nullptr;
  const RegionAmbience* regions_ = nullptr;
  std::string current_path_;
  u32 current_voice_ = 0;
};

}  // namespace rec::audio

#endif  // RECREATION_AUDIO_AMBIENT_H_
