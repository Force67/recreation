#include "audio/ambient.h"

#include <cstring>

#include "audio/audio_system.h"
#include "bethesda/load_order.h"
#include "bethesda/record.h"
#include "core/log.h"

namespace rec::audio {
namespace {

constexpr u32 kRegn = FourCc('R', 'E', 'G', 'N');
constexpr u32 kRdat = FourCc('R', 'D', 'A', 'T');
constexpr u32 kRdsa = FourCc('R', 'D', 'S', 'A');
constexpr u32 kRdsd = FourCc('R', 'D', 'S', 'D');

// One region sound entry is a sound form id (u32), a flags word, and a chance
// word -- twelve bytes. The list lives in RDSA (Skyrim SE) or RDSD (older).
constexpr size_t kSoundEntryStride = 12;

// The ambient bed plays as a 2D loop: it is the air of the place, not a point
// source, so it sits evenly in both ears and fades in over a couple of seconds.
constexpr f32 kAmbientGain = 0.55f;
constexpr f32 kAmbientFadeIn = 2.0f;
constexpr f32 kAmbientFadeOut = 1.5f;

}  // namespace

void RegionAmbience::Build(const bethesda::RecordStore& records) {
  region_sounds_.clear();
  records.EachOfType(kRegn, [&](bethesda::GlobalFormId id,
                                const bethesda::RecordStore::StoredRecord& stored) {
    bethesda::Record record;
    if (!records.Parse(id, &record)) return;
    std::vector<bethesda::GlobalFormId> sounds;
    // Subrecords are ordered into RDAT-led data sections; the sound entries are
    // an RDSA/RDSD array. Reading them wherever they appear keeps this tolerant
    // of the section-type differences between the games.
    for (const bethesda::Subrecord& sub : record.subrecords) {
      if (sub.type != kRdsa && sub.type != kRdsd) continue;
      for (size_t off = 0; off + kSoundEntryStride <= sub.data.size(); off += kSoundEntryStride) {
        u32 raw;
        std::memcpy(&raw, sub.data.data() + off, 4);
        const bethesda::GlobalFormId sound =
            records.ResolveFrom(bethesda::RawFormId{raw}, stored.winning_plugin);
        if (sound.plugin != 0xffff && sound.local_id != 0) sounds.push_back(sound);
      }
    }
    if (!sounds.empty()) region_sounds_[id.packed()] = std::move(sounds);
  });
  REC_INFO("audio: {} regions carry ambient sounds", region_sounds_.size());
}

const std::vector<bethesda::GlobalFormId>& RegionAmbience::SoundsFor(u64 region) const {
  auto it = region_sounds_.find(region);
  return it != region_sounds_.end() ? it->second : empty_;
}

AmbientDecision DecideAmbient(const std::string& current, const std::string& target) {
  if (current == target) return {};  // already on the right bed (or both silent)
  AmbientDecision decision;
  decision.stop_current = !current.empty();
  decision.start_target = !target.empty();
  return decision;
}

std::string AmbientDirector::Resolve(const AmbientContext& context) const {
  if (!catalog_ || !regions_) return {};
  // Exterior region ambience: the first of the region's sounds that resolves to a
  // file the Vfs can load. Interiors fall back to their own systems (acoustic
  // spaces / cell music), which stay silent here rather than play the outdoors.
  if (context.interior || context.region == 0) return {};
  for (const bethesda::GlobalFormId& form : regions_->SoundsFor(context.region)) {
    std::string path = catalog_->PathFor(form);
    if (!path.empty()) return path;
  }
  return {};
}

void AmbientDirector::Update(const AmbientContext& context) {
  if (!audio_) return;
  const std::string target = Resolve(context);
  const AmbientDecision decision = DecideAmbient(current_path_, target);
  if (!decision.stop_current && !decision.start_target) return;  // unchanged

  if (decision.stop_current && current_voice_) {
    audio_->Stop(current_voice_, kAmbientFadeOut);
    current_voice_ = 0;
  }
  if (decision.start_target) {
    PlayParams params;
    params.gain = kAmbientGain;
    params.positional = false;
    params.fade_in = kAmbientFadeIn;
    current_voice_ = audio_->PlayLoop(target, params);
    if (current_voice_) REC_INFO("audio: ambient bed -> {}", target);
  }
  current_path_ = target;
}

void AmbientDirector::Stop() {
  if (audio_ && current_voice_) audio_->Stop(current_voice_, kAmbientFadeOut);
  current_voice_ = 0;
  current_path_.clear();
}

}  // namespace rec::audio
