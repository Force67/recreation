#ifndef RECREATION_ASSET_PRIMITIVES_H_
#define RECREATION_ASSET_PRIMITIVES_H_

#include "asset/mesh.h"
#include "asset/skeleton.h"

namespace rec::asset {

// Procedural test shapes for bringup and unit tests.
Mesh MakeCube(f32 half_extent, AssetId id);

// A uv sphere with smooth normals, tangents and equirect uvs. One empty
// submesh is appended so the caller only has to set its material. Used by the
// material preview scene where clearcoat/sheen/anisotropy read best on a curve.
Mesh MakeSphere(f32 radius, u32 rings, u32 segments, AssetId id);

// A blocky biped: a skeleton with the standard Skyrim bone names (so the
// procedural locomotion drives it) and a skinned box-limb mesh bound to it,
// authored in engine space (meters, Y-up). For bringup of the skinning,
// animation and foot IK paths without game data.
void MakeSkinnedBiped(AssetId mesh_id, Skeleton* out_skeleton, Mesh* out_mesh);

}  // namespace rec::asset

#endif  // RECREATION_ASSET_PRIMITIVES_H_
