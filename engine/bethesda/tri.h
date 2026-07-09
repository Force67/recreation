#ifndef RECREATION_BETHESDA_TRI_H_
#define RECREATION_BETHESDA_TRI_H_

#include <optional>
#include <string>
#include <string_view>

#include <base/containers/vector.h>

#include "core/types.h"

namespace rx::bethesda {

// Skyrim head/hair/brow/beard morph file ("FRTRI003"). Ships the base head
// shape plus named vertex morphs: race blends (NordRace, BretonRace, ...),
// chargen slider morphs (NoseUp, BrowBack, ...) and facial expressions. HDPT
// records point at the race tri (NAM0) and chargen tri (NAM1); NPC_ NAM9 face
// values weight the chargen morphs.
//
// Verified layout against Skyrim SE data (malehead.tri, femaleheadchargen.tri,
// races/expression/hair/beard tris all consume to the exact byte):
//   char[8]  magic "FRTRI003"
//   u32      vertex_count
//   u32      face_count            (triangles)
//   u32      unknown_0c            (always 0 in SE)
//   u32      unknown_10            (always 0 in SE)
//   u32      unknown_14            (always 0 in SE)
//   u32      uv_count              (== vertex_count when flags bit0 set)
//   u32      flags                 (bit0: has UVs)
//   u32      morph_count
//   u32      modifier_count        (always 0 in SE)
//   u32      modifier_vertex_count (always 0 in SE)
//   u32      unknown_30..3c        (4x u32, always 0 in SE)
//   f32[vertex_count*3]            base vertices (Bethesda object space)
//   u32[face_count*3]              vertex indices
//   if flags bit0:
//     f32[uv_count*2]              uvs
//     u32[face_count*3]            uv indices
//   morph_count times:
//     u32                          name length (includes the null terminator)
//     char[name length]            name (null terminated)
//     f32                          scale (delta multiplier)
//     i16[vertex_count*3]          xyz deltas; world delta = scale * value
struct TriDelta {
  i16 x = 0;
  i16 y = 0;
  i16 z = 0;
};

struct TriMorph {
  std::string name;
  f32 scale = 0;  // per-morph multiplier: applied delta = scale * TriDelta
  base::Vector<TriDelta> deltas;  // one per base vertex
};

struct TriMorphSet {
  u32 vertex_count = 0;
  base::Vector<TriMorph> morphs;
  // FaceGen "modifier" morphs. Skyrim SE ships none (modifier_count is always
  // 0), so this stays empty and the trailing modifier block, whose layout no
  // SE asset exercises, is left unparsed rather than guessed.
  base::Vector<TriMorph> modifiers;

  // Case sensitive exact match; null when absent.
  const TriMorph* FindMorph(std::string_view name) const;
};

// Parses a Skyrim FRTRI003 blob. Bounds checks every field and returns an
// empty optional on a bad magic, an overrun, or an implausible count.
std::optional<TriMorphSet> ParseTri(ByteSpan data);

// Adds a weighted morph into an xyz-interleaved position array in place.
// `positions` holds vertex_count*3 floats in the tri's vertex order. Applies
// min(vertex_count, morph.deltas.size()) vertices so a mismatched mesh cannot
// overrun.
void ApplyMorph(const TriMorph& morph, f32 weight, f32* positions, u32 vertex_count);

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_TRI_H_
