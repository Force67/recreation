#include "audio/ambient.h"

#include <cstring>

#include "audio/audio_system.h"
#include "bethesda/load_order.h"
#include "bethesda/record.h"
#include "core/log.h"

namespace rec::audio {
namespace {

constexpr u32 kRegn = FourCc('R', 'E', 'G', 'N');
constexpr u32 kWnam = FourCc('W', 'N', 'A', 'M');
constexpr u32 kRpld = FourCc('R', 'P', 'L', 'D');
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

// Even-odd ray cast: is (x, y) inside the polygon?
bool PointInPolygon(const std::vector<std::pair<f32, f32>>& poly, f32 x, f32 y) {
  bool inside = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const f32 xi = poly[i].first, yi = poly[i].second;
    const f32 xj = poly[j].first, yj = poly[j].second;
    if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi)) inside = !inside;
  }
  return inside;
}

}  // namespace

void RegionAmbience::Build(const bethesda::RecordStore& records,
                           bethesda::GlobalFormId worldspace) {
  regions_.clear();
  records.EachOfType(kRegn, [&](bethesda::GlobalFormId id,
                                const bethesda::RecordStore::StoredRecord& stored) {
    bethesda::Record record;
    if (!records.Parse(id, &record)) return;

    // Restrict to the active worldspace (a region's WNAM), so polygons from other
    // worldspaces cannot falsely match the player's position.
    if (worldspace.plugin != 0xffff) {
      const bethesda::Subrecord* wnam = record.Find(kWnam);
      if (!wnam || wnam->data.size() < 4) return;
      u32 raw;
      std::memcpy(&raw, wnam->data.data(), 4);
      if (!(records.ResolveFrom(bethesda::RawFormId{raw}, stored.winning_plugin) == worldspace))
        return;
    }

    Region region;
    region.form = id.packed();
    // Subrecords are ordered: the area polygon (RPLD) precedes the RDAT-led data
    // sections; the sound entries are an RDSA/RDSD array. Reading the sounds
    // wherever they appear keeps this tolerant of the games' section differences.
    for (const bethesda::Subrecord& sub : record.subrecords) {
      if (sub.type == kRpld) {
        for (size_t k = 0; k + 8 <= sub.data.size(); k += 8) {
          f32 px, py;
          std::memcpy(&px, sub.data.data() + k, 4);
          std::memcpy(&py, sub.data.data() + k + 4, 4);
          region.polygon.push_back({px, py});
        }
      } else if (sub.type == kRdat) {
        if (sub.data.size() >= 6) region.priority = sub.data.data()[5];
      } else if (sub.type == kRdsa || sub.type == kRdsd) {
        for (size_t off = 0; off + kSoundEntryStride <= sub.data.size(); off += kSoundEntryStride) {
          u32 raw;
          std::memcpy(&raw, sub.data.data() + off, 4);
          const bethesda::GlobalFormId sound =
              records.ResolveFrom(bethesda::RawFormId{raw}, stored.winning_plugin);
          if (sound.plugin != 0xffff && sound.local_id != 0) region.sounds.push_back(sound);
        }
      }
    }
    if (region.polygon.size() >= 3 && !region.sounds.empty()) regions_.push_back(std::move(region));
  });
  REC_INFO("audio: {} regions carry ambient sounds", regions_.size());
}

u64 RegionAmbience::RegionAt(f32 x, f32 y) const {
  u64 best = 0;
  i32 best_priority = -1;
  for (const Region& region : regions_) {
    if (region.priority < best_priority) continue;
    if (PointInPolygon(region.polygon, x, y)) {
      best = region.form;
      best_priority = region.priority;
    }
  }
  return best;
}

const std::vector<bethesda::GlobalFormId>& RegionAmbience::SoundsFor(u64 region) const {
  for (const Region& r : regions_)
    if (r.form == region) return r.sounds;
  return empty_;
}

std::vector<u64> RegionAmbience::RegionForms() const {
  std::vector<u64> forms;
  forms.reserve(regions_.size());
  for (const Region& r : regions_) forms.push_back(r.form);
  return forms;
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
    if (path.empty()) continue;
    // Skip a sound whose file is not actually present, so a region with a missing
    // first variation still falls through to one that plays instead of going mute.
    if (audio_ && !audio_->HasAsset(path)) continue;
    return path;
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
