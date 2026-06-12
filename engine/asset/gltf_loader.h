#ifndef RECREATION_ASSET_GLTF_LOADER_H_
#define RECREATION_ASSET_GLTF_LOADER_H_

#include <string>

#include <base/containers/vector.h>

#include "asset/material.h"
#include "asset/mesh.h"
#include "asset/texture.h"
#include "core/math.h"

namespace rec::asset {

// A flattened glTF scene: node transforms baked to world space, one engine
// Mesh per glTF mesh (primitives become submeshes), textures decoded to
// rgba8. Asset ids derive from "<path>#<kind><index>" so scenes from
// different files never collide.
struct GltfScene {
  base::Vector<Texture> textures;
  base::Vector<Material> materials;
  base::Vector<Mesh> meshes;

  struct Instance {
    u32 mesh_index = 0;
    Vec3 position{};
    f32 rotation[4] = {0, 0, 0, 1};  // quaternion x y z w
    f32 scale = 1.0f;                // uniform; non uniform scale is averaged
  };
  base::Vector<Instance> instances;
};

// Loads .gltf or .glb including external buffers and images. Generates
// tangents from uv derivatives when the source has none. Returns false and
// logs on malformed input.
bool LoadGltfScene(const std::string& path, GltfScene* out);

}  // namespace rec::asset

#endif  // RECREATION_ASSET_GLTF_LOADER_H_
