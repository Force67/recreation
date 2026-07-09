// Sound natives backed by the engine audio system. A Papyrus Sound is a SOUN
// form; the catalog resolves it to an asset path that the audio system plays.
#include "audio/audio_system.h"
#include "script/games/skyrim/skyrim_bindings.h"

namespace rx::script::skyrim {

using papyrus::ObjectRef;

i32 RecordBackedSkyrimBindings::PlaySound(ObjectRef sound, ObjectRef /*source*/) {
  if (!audio_ || !records_) return 0;
  if (!sound_catalog_built_) {
    sound_catalog_.Build(*records_);
    sound_catalog_built_ = true;
  }
  std::string path = sound_catalog_.PathFor(ToFormId(sound));
  if (path.empty()) return 0;
  // A 2D one-shot for now; spatialising it at the source is a later refinement.
  return static_cast<i32>(audio_->PlayUi(path));
}

void RecordBackedSkyrimBindings::StopSoundInstance(i32 instance) {
  if (audio_ && instance > 0) audio_->Stop(static_cast<u32>(instance));
}

void RecordBackedSkyrimBindings::SetSoundInstanceVolume(i32 instance, f32 volume) {
  if (audio_ && instance > 0) audio_->SetVoiceGain(static_cast<u32>(instance), volume);
}

void RecordBackedSkyrimBindings::SetSoundCategoryVolume(ObjectRef /*category*/, f32 volume) {
  // No per-category bus yet, so a category change scales the master volume.
  if (audio_) audio_->SetMasterVolume(volume);
}

}  // namespace rx::script::skyrim
