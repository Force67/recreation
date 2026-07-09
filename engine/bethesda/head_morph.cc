#include "bethesda/head_morph.h"

#include <algorithm>
#include <cmath>

namespace rx::bethesda {
namespace {

// NAM9 slider -> chargen morph pair, in NAM9 file order. Verified against
// MaleHeadCustomizations.tri: its 35 directional morphs form exactly these 18
// opposing pairs (JawForward is single-sided; no JawBack morph ships).
const Nam9Slider kSliders[kNam9Count] = {
    {"NoseLong", "NoseLong", "NoseShort"},       // 0
    {"NoseUp", "NoseUp", "NoseDown"},            // 1
    {"JawUp", "JawUp", "JawDown"},               // 2
    {"JawWide", "JawWide", "JawNarrow"},         // 3
    {"JawForward", "JawForward", nullptr},       // 4 (single-sided)
    {"CheeksUp", "CheeksUp", "CheeksDown"},      // 5
    {"CheeksBack", "CheeksIn", "CheeksOut"},     // 6
    {"EyesUp", "EyesMoveUp", "EyesMoveDown"},    // 7
    {"EyesIn", "EyesMoveIn", "EyesMoveOut"},     // 8
    {"BrowsUp", "BrowUp", "BrowDown"},           // 9
    {"BrowsIn", "BrowIn", "BrowOut"},            // 10
    {"BrowsBack", "BrowBack", "BrowForward"},    // 11
    {"LipsUp", "LipMoveUp", "LipMoveDown"},      // 12
    {"LipsOut", "LipMoveOut", "LipMoveIn"},      // 13
    {"ChinWide", "ChinWide", "ChinThin"},        // 14
    {"ChinUp", "ChinMoveUp", "ChinMoveDown"},    // 15
    {"ChinUnderbite", "Underbite", "Overbite"},  // 16
    {"EyesForward", "EyesForward", "EyesBack"},  // 17
};

void ApplyMorphToLod(const TriMorph& morph, f32 weight, asset::MeshLod& lod) {
  const u32 n = std::min(static_cast<u32>(lod.vertices.size()),
                         static_cast<u32>(morph.deltas.size()));
  const f32 s = weight * morph.scale;
  for (u32 i = 0; i < n; ++i) {
    const TriDelta& d = morph.deltas[i];
    lod.vertices[i].position[0] += s * static_cast<f32>(d.x);
    lod.vertices[i].position[1] += s * static_cast<f32>(d.y);
    lod.vertices[i].position[2] += s * static_cast<f32>(d.z);
  }
}

}  // namespace

const Nam9Slider& Nam9SliderInfo(u32 index) {
  static const Nam9Slider kNone{"?", nullptr, nullptr};
  return index < kNam9Count ? kSliders[index] : kNone;
}

void CollectFaceMorphs(const f32 nam9[kNam9Count], const i32 nama[4],
                       base::Vector<MorphWeight>* out) {
  for (u32 i = 0; i < kNam9Count; ++i) {
    f32 v = nam9[i];
    if (!std::isfinite(v) || std::fabs(v) < 1e-4f) continue;
    const Nam9Slider& s = kSliders[i];
    if (v >= 0) {
      out->push_back({s.positive, v});
    } else if (s.negative) {
      out->push_back({s.negative, -v});
    }
  }
  // NAMA type morphs, applied at full strength. Slot 1 (brows) picks a separate
  // head part, not a morph.
  const char* kTypePrefix[4] = {"NoseType", nullptr, "EyesType", "LipType"};
  for (int slot = 0; slot < 4; ++slot) {
    if (!kTypePrefix[slot]) continue;
    i32 idx = nama[slot];
    if (idx < 1) continue;  // -1 = none, 0 = the base type (no morph)
    out->push_back({std::string(kTypePrefix[slot]) + std::to_string(idx), 1.0f});
  }
}

bool ApplyHeadMorphs(asset::MeshLod& lod, const TriMorphSet* race_tri,
                     const std::string& race_morph, const TriMorphSet* chargen_tri,
                     const base::Vector<MorphWeight>& chargen) {
  const u32 verts = static_cast<u32>(lod.vertices.size());
  bool applied = false;
  if (race_tri && race_tri->vertex_count == verts && !race_morph.empty()) {
    if (const TriMorph* m = race_tri->FindMorph(race_morph)) {
      ApplyMorphToLod(*m, 1.0f, lod);
      applied = true;
    }
  }
  if (chargen_tri && chargen_tri->vertex_count == verts) {
    for (const MorphWeight& w : chargen) {
      if (const TriMorph* m = chargen_tri->FindMorph(w.name)) {
        ApplyMorphToLod(*m, w.weight, lod);
        applied = true;
      }
    }
  }
  return applied;
}

}  // namespace rx::bethesda
