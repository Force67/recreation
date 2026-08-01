#include "runtime/vehicle/cart_visuals.h"

#include <cmath>
#include <string>

#include "asset/primitives.h"

namespace rx::cart {
namespace {

// Bethesda Z-up game units -> engine Y-up metres, matching the actor rigs.
constexpr f32 kBethScale = 0.01428f;

Vec3 BethToEngine(f32 x, f32 y, f32 z) {
  return {x * kBethScale, z * kBethScale, -y * kBethScale};
}

}  // namespace

bool BakeBody(asset::AssetDatabase* assets, render::Renderer* renderer, const char* path,
              bool recenter, const char* id, asset::AssetId* out) {
  if (!assets) return false;
  const asset::Mesh* src = assets->LoadMesh(path && path[0] ? path : kBodyMesh);
  if (!src || src->lods.empty()) return false;
  const Vec3 c = recenter ? BethToEngine(src->bounds_center[0], src->bounds_center[1],
                                         src->bounds_center[2])
                          : Vec3{};
  asset::Mesh baked = *src;
  baked.id = asset::MakeAssetId(id);
  baked.skinned = false;
  baked.skin = {};
  for (asset::MeshLod& lod : baked.lods) {
    for (asset::Vertex& v : lod.vertices) {
      const Vec3 p = BethToEngine(v.position[0], v.position[1], v.position[2]);
      v.position[0] = p.x - c.x;
      v.position[1] = p.y - c.y;
      v.position[2] = p.z - c.z;
      const Vec3 n = BethToEngine(v.normal[0], v.normal[1], v.normal[2]);
      v.normal[0] = n.x;
      v.normal[1] = n.y;
      v.normal[2] = n.z;
    }
  }
  const Vec3 center = BethToEngine(src->bounds_center[0], src->bounds_center[1],
                                   src->bounds_center[2]);
  baked.bounds_center[0] = center.x - c.x;
  baked.bounds_center[1] = center.y - c.y;
  baked.bounds_center[2] = center.z - c.z;
  baked.bounds_radius = src->bounds_radius * kBethScale;
  if (renderer) {
    // The cell streamer normally does this for a placed ref; a mesh loaded
    // straight out of the database has to push its own textures and materials
    // or it draws untextured white.
    for (const asset::MeshLod& lod : baked.lods) {
      for (const asset::Submesh& submesh : lod.submeshes) {
        const asset::Material* material = assets->FindMaterial(submesh.material);
        if (!material) continue;
        for (asset::AssetId texture_id : {material->base_color, material->normal,
                                          material->metallic_roughness, material->emissive}) {
          if (!texture_id) continue;
          if (const asset::Texture* texture = assets->FindTexture(texture_id))
            renderer->UploadTexture(*texture);
        }
        renderer->UploadMaterial(*material);
      }
    }
    renderer->UploadMesh(baked);
  }
  *out = baked.id;
  return true;
}

asset::Mesh MakeWheel(render::Renderer* renderer, const char* id, f32 radius, f32 half_width,
                      bool upload) {
  asset::Material mat;
  mat.id = asset::MakeAssetId(std::string(id) + "/mat");
  mat.base_color_factor[0] = 0.12f;
  mat.base_color_factor[1] = 0.09f;
  mat.base_color_factor[2] = 0.07f;
  mat.roughness_factor = 0.9f;

  asset::Mesh mesh;
  mesh.id = asset::MakeAssetId(id);
  asset::MeshLod lod;
  constexpr u32 kSeg = 20;
  auto push = [&](f32 x, f32 y, f32 z, f32 nx, f32 ny, f32 nz) {
    asset::Vertex v{};
    v.position[0] = x;
    v.position[1] = y;
    v.position[2] = z;
    v.normal[0] = nx;
    v.normal[1] = ny;
    v.normal[2] = nz;
    v.tangent[0] = 0;
    v.tangent[1] = 0;
    v.tangent[2] = 1;
    v.tangent[3] = 1;
    lod.vertices.push_back(v);
  };
  // Side ring (two rings of kSeg, quads between them).
  for (u32 k = 0; k < kSeg; ++k) {
    const f32 a = 6.2831853f * static_cast<f32>(k) / kSeg;
    const f32 cy = std::cos(a), sz = std::sin(a);
    push(-half_width, radius * cy, radius * sz, 0, cy, sz);
    push(half_width, radius * cy, radius * sz, 0, cy, sz);
  }
  for (u32 k = 0; k < kSeg; ++k) {
    const u32 a0 = k * 2, a1 = ((k + 1) % kSeg) * 2;
    lod.indices.push_back(a0);
    lod.indices.push_back(a1);
    lod.indices.push_back(a0 + 1);
    lod.indices.push_back(a1);
    lod.indices.push_back(a1 + 1);
    lod.indices.push_back(a0 + 1);
  }
  // Two caps.
  for (int side = 0; side < 2; ++side) {
    const f32 x = side ? half_width : -half_width;
    const f32 nx = side ? 1.0f : -1.0f;
    const u32 center = static_cast<u32>(lod.vertices.size());
    push(x, 0, 0, nx, 0, 0);
    const u32 ring0 = static_cast<u32>(lod.vertices.size());
    for (u32 k = 0; k < kSeg; ++k) {
      const f32 a = 6.2831853f * static_cast<f32>(k) / kSeg;
      push(x, radius * std::cos(a), radius * std::sin(a), nx, 0, 0);
    }
    for (u32 k = 0; k < kSeg; ++k) {
      const u32 v0 = ring0 + k, v1 = ring0 + (k + 1) % kSeg;
      if (side) {
        lod.indices.push_back(center);
        lod.indices.push_back(v0);
        lod.indices.push_back(v1);
      } else {
        lod.indices.push_back(center);
        lod.indices.push_back(v1);
        lod.indices.push_back(v0);
      }
    }
  }
  lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), mat.id});
  mesh.bounds_radius = std::sqrt(radius * radius + half_width * half_width);
  mesh.lods.push_back(std::move(lod));
  if (upload && renderer) {
    renderer->UploadMaterial(mat);
    renderer->UploadMesh(mesh);
  }
  return mesh;
}

asset::Mesh MakeBox(render::Renderer* renderer, const char* id, Vec3 half, f32 r, f32 g, f32 b,
                    bool upload) {
  asset::Material mat;
  mat.id = asset::MakeAssetId(std::string(id) + "/mat");
  mat.base_color_factor[0] = r;
  mat.base_color_factor[1] = g;
  mat.base_color_factor[2] = b;
  mat.roughness_factor = 0.8f;
  asset::Mesh mesh = asset::MakeBox(half.x, half.y, half.z, asset::MakeAssetId(id));
  for (asset::MeshLod& lod : mesh.lods) {
    if (lod.submeshes.empty())
      lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), mat.id});
    else
      for (asset::Submesh& sm : lod.submeshes) sm.material = mat.id;
  }
  if (upload && renderer) {
    renderer->UploadMaterial(mat);
    renderer->UploadMesh(mesh);
  }
  return mesh;
}

}  // namespace rx::cart
