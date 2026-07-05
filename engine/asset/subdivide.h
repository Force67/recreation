#ifndef RECREATION_ASSET_SUBDIVIDE_H_
#define RECREATION_ASSET_SUBDIVIDE_H_

#include "asset/mesh.h"
#include "core/types.h"

namespace rec::asset {

// Recomputes per-vertex normals (area-weighted face normals) and tangents (uv
// gradient, Gram-Schmidt orthonormalized, handedness stored in tangent.w) for a
// triangle-list mesh lod, in place. Call after any operation that moves
// positions (morphing, subdivision).
void RecomputeNormalsTangents(MeshLod& lod);

// Loop-subdivides a triangle-list mesh lod `levels` times, in place, then
// recomputes normals/tangents. Interior vertices/edges use the smoothing Loop
// rules; mesh boundary vertices (open edges: the neck ring, eye/mouth holes,
// part borders) are held at their input positions and boundary edges split at
// their linear midpoint, so separately authored parts that share a border still
// line up afterwards. UVs and vertex colors are linearly interpolated at new
// vertices; the per-material submesh partition is preserved. `levels` == 0 only
// recomputes normals/tangents.
void SubdivideLoop(MeshLod& lod, u32 levels);

}  // namespace rec::asset

#endif  // RECREATION_ASSET_SUBDIVIDE_H_
