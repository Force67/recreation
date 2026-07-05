#ifndef RECREATION_BETHESDA_HEAD_MORPH_H_
#define RECREATION_BETHESDA_HEAD_MORPH_H_

#include <string>

#include <base/containers/vector.h>

#include "asset/mesh.h"
#include "bethesda/tri.h"
#include "core/types.h"

namespace rec::bethesda {

// Runtime FaceGen head morphing. Maps an NPC_'s NAM9 face sliders + NAMA
// face-part type indices onto the named morphs in the head-part chargen/race
// tris and applies them to the head mesh.
//
// The 18 usable NAM9 sliders (the 19th float is FLT_MAX junk) each carry a
// signed weight (~ -1..1) that drives a positive/negative pair of chargen tri
// morphs; a couple are single-sided. Established against Skyrim SE data: the
// head chargen tri (MaleHeadCustomizations.tri) ships exactly the 18 opposing
// morph pairs the sliders below name, and the NAMA indices select the
// NoseType/EyesType/LipType morphs in that same tri.
constexpr u32 kNam9Count = 18;

// A resolved tri morph request: a morph name plus the weight to apply it at.
struct MorphWeight {
  std::string name;
  f32 weight = 0;
};

// One NAM9 slider's chargen morph pair, for UI labels and enumeration.
struct Nam9Slider {
  const char* label;     // NAM9 slider name, e.g. "NoseUp"
  const char* positive;  // chargen morph applied when the value is >= 0
  const char* negative;  // chargen morph applied when the value is < 0, or null
};
const Nam9Slider& Nam9SliderInfo(u32 index);  // index < kNam9Count

// Turns an NPC's NAM9 slider values + NAMA type indices into chargen tri morph
// requests. NAMA slots 0/2/3 (nose/eyes/mouth) select a NoseType/EyesType/
// LipType morph at full weight; -1 or 0 = none. Brows (NAMA slot 1) select a
// separate head part, not a head morph, so are not emitted here.
void CollectFaceMorphs(const f32 nam9[kNam9Count], const i32 nama[4],
                       base::Vector<MorphWeight>* out);

// Applies morphs to a head-part mesh lod in place: the race blend (the morph
// named `race_morph` in `race_tri`, weight 1) first, then every `chargen`
// request found in `chargen_tri`. A tri that lacks a requested morph (e.g. brows
// carry no NoseType) simply skips it. A tri whose vertex count does not match
// the lod is ignored (its deltas index the source-NIF vertex order, so a
// mismatch would garble the mesh). Returns true if any morph was applied. Does
// not recompute normals: call asset::RecomputeNormalsTangents or SubdivideLoop
// after.
bool ApplyHeadMorphs(asset::MeshLod& lod, const TriMorphSet* race_tri,
                     const std::string& race_morph, const TriMorphSet* chargen_tri,
                     const base::Vector<MorphWeight>& chargen);

}  // namespace rec::bethesda

#endif  // RECREATION_BETHESDA_HEAD_MORPH_H_
