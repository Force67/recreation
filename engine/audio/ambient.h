#ifndef RECREATION_AUDIO_AMBIENT_H_
#define RECREATION_AUDIO_AMBIENT_H_

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "audio/sound_catalog.h"
#include "bethesda/form_id.h"
#include "core/types.h"

namespace rx::bethesda {
class RecordStore;
}

namespace rx::audio {

class AudioSystem;

// REGN region -> its authored ambient sounds, with the region's worldspace
// polygon. A Skyrim region carries a weighted list of looping ambiences (the wind
// on the tundra, the gulls at the coast) and the area it covers; we parse both so
// the director can resolve, on its own, which region the player stands in. This
// is independent of the weather system's regions, which the engine may pin off.
class RegionAmbience {
 public:
  // Parses every REGN that belongs to `worldspace` (pass a 0xffff-plugin id to
  // accept all) and has both an area polygon and ambient sounds.
  void Build(const bethesda::RecordStore& records, bethesda::GlobalFormId worldspace);

  // The region (its REGN form, packed) whose polygon contains the worldspace
  // point (game units, x/y), or 0 when the point is in no ambient region. Higher
  // priority wins where regions overlap.
  u64 RegionAt(f32 x, f32 y) const;

  // The ambient sound forms for a region (resolve them through a SoundCatalog),
  // or an empty list when the region has none.
  const std::vector<bethesda::GlobalFormId>& SoundsFor(u64 region) const;

  // The packed form id of every region carrying ambient sounds (for diagnostics).
  std::vector<u64> RegionForms() const;

  bool empty() const { return regions_.empty(); }
  size_t size() const { return regions_.size(); }

 private:
  struct Region {
    u64 form = 0;
    i32 priority = 0;
    std::vector<std::pair<f32, f32>> polygon;  // worldspace XY, game units
    std::vector<bethesda::GlobalFormId> sounds;
  };
  std::vector<Region> regions_;
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

}  // namespace rx::audio

#endif  // RECREATION_AUDIO_AMBIENT_H_
