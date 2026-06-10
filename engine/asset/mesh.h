#ifndef RECREATION_ASSET_MESH_H_
#define RECREATION_ASSET_MESH_H_

#include <vector>

#include "asset/asset_id.h"
#include "core/types.h"

namespace rec::asset {

// Engine native mesh. Interleaved vertex streams as the source formats give
// no benefit from keeping their layout, GPU friendly index buffer, submeshes
// per material. NIF geometry is converted into this at load time.
struct Vertex {
  f32 position[3];
  f32 normal[3];
  f32 tangent[4];
  f32 uv[2];
  u32 color = 0xffffffff;
};

struct SkinnedVertexExtra {
  u8 bone_indices[4];
  u8 bone_weights[4];
};

struct Submesh {
  u32 index_offset = 0;
  u32 index_count = 0;
  AssetId material;
};

struct MeshLod {
  std::vector<Vertex> vertices;
  std::vector<SkinnedVertexExtra> skinning;
  std::vector<u32> indices;
  std::vector<Submesh> submeshes;
};

struct Mesh {
  AssetId id;
  std::vector<MeshLod> lods;
  f32 bounds_center[3] = {0, 0, 0};
  f32 bounds_radius = 0;
};

}  // namespace rec::asset

#endif  // RECREATION_ASSET_MESH_H_
