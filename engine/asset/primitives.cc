#include "asset/primitives.h"

namespace rec::asset {

Mesh MakeCube(f32 half_extent, AssetId id) {
  struct Face {
    f32 n[3];  // normal
    f32 u[3];  // tangent axes, cross(u, v) == n so corners wind ccw
    f32 v[3];
  };
  static constexpr Face kFaces[] = {
      {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},   {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
      {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}},   {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
      {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},   {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},
  };
  static constexpr f32 kCorners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
  static constexpr f32 kUvs[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

  Mesh mesh;
  mesh.id = id;
  MeshLod& lod = mesh.lods.emplace_back();
  lod.vertices.reserve(24);
  lod.indices.reserve(36);

  for (const Face& face : kFaces) {
    u32 base = static_cast<u32>(lod.vertices.size());
    for (int corner = 0; corner < 4; ++corner) {
      Vertex vertex{};
      for (int axis = 0; axis < 3; ++axis) {
        vertex.position[axis] = half_extent * (face.n[axis] + kCorners[corner][0] * face.u[axis] +
                                               kCorners[corner][1] * face.v[axis]);
        vertex.normal[axis] = face.n[axis];
        vertex.tangent[axis] = face.u[axis];
      }
      vertex.tangent[3] = 1.0f;
      vertex.uv[0] = kUvs[corner][0];
      vertex.uv[1] = kUvs[corner][1];
      lod.vertices.push_back(vertex);
    }
    for (u32 index : {0u, 1u, 2u, 0u, 2u, 3u}) lod.indices.push_back(base + index);
  }

  mesh.bounds_radius = half_extent * 1.7320508f;
  return mesh;
}

}  // namespace rec::asset
