#ifndef RECREATION_RENDER_HAIR_GROOM_H_
#define RECREATION_RENDER_HAIR_GROOM_H_

#include <base/containers/vector.h>

#include "asset/mesh.h"
#include "asset/texture.h"
#include "core/math.h"

namespace rec::render {

// Guide-strand grooming from a real Bethesda hair mesh. Hair NIFs are card
// (shell) meshes whose UVs run root->tip along one axis; we resample each card
// into strands that trace it from the scalp edge to the tip, so the groom keeps
// the original silhouette instead of dissolving into a fuzzball.

static constexpr u32 kGroomPointsPerStrand = 16;

struct GroomParams {
  u32 guide_count = 1600;         // simulated guide strands
  u32 children_per_guide = 6;     // rendered clump children per guide (M > N)
  f32 clump_radius = 0.006f;      // child clump radius at the root (engine units)
  f32 strand_width = 0.0016f;     // ribbon half-width at the root (engine units)
  Vec3 tint{1, 1, 1};             // multiplies the sampled diffuse colour
  const asset::Texture* diffuse = nullptr;  // hair diffuse for per-strand colour
  u32 seed = 1;
  // Scales the per-strand flyaway + curl amplitude. 1 = the demo's loose,
  // wind-blown look; lower values keep strands tight to the card silhouette for
  // groomed styles (facegen heads read as spikes at full frizz).
  f32 frizz = 1.0f;
  // When true (the default) the groom is recentred so the scalp sits at the local
  // origin, ready for a translation. Bone-attached hair keeps its authored
  // (engine-scaled, head-local) coordinates instead, so the caller can drop it on
  // the head bone with the head part's own transform.
  bool recenter = true;
};

// A CPU-built groom in a groom-local frame: engine units, Y-up, recentred so the
// scalp sits at the local origin. A root transform drops it onto a head bone;
// `collision_*` is that head sphere in the same local frame.
struct GroomData {
  base::Vector<f32> points;  // guide_count * kGroomPointsPerStrand * 3 (xyz)
  base::Vector<f32> roots;   // guide_count * 3 (local root pos, for sim reset)
  base::Vector<f32> colors;  // guide_count * 3 (linear rgb)
  u32 guide_count = 0;
  Vec3 collision_center{0, 0, 0};
  f32 collision_radius = 0;
  f32 mean_length = 0;  // average strand length, for diagnostics
  Vec3 authored_scalp{0, 0, 0};  // scalp mean before recentring (local engine units)
};

// Traces guide strands along the hair cards. Returns false when the mesh has no
// usable geometry.
bool BuildHairGroom(const asset::Mesh& hair_mesh, const GroomParams& params, GroomData* out);

}  // namespace rec::render

#endif  // RECREATION_RENDER_HAIR_GROOM_H_
