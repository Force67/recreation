#ifndef RECREATION_ASSET_PRIMITIVES_H_
#define RECREATION_ASSET_PRIMITIVES_H_

#include "asset/mesh.h"

namespace rec::asset {

// Procedural test shapes for bringup and unit tests.
Mesh MakeCube(f32 half_extent, AssetId id);

}  // namespace rec::asset

#endif  // RECREATION_ASSET_PRIMITIVES_H_
