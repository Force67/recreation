#ifndef RECREATION_RENDER_RHI_RESOURCES_H_
#define RECREATION_RENDER_RHI_RESOURCES_H_

#include <volk.h>

#include <vk_mem_alloc.h>

#include <base/containers/vector.h>

#include "core/types.h"

namespace rec::render {

struct GpuBuffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;
  u64 size = 0;
  void* mapped = nullptr;  // set for host visible buffers
};

struct GpuImage {
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkExtent2D extent{};
};

// Index range drawn with one material. Materials resolve at upload time so
// the draw loop is a plain array walk.
struct GpuSubmesh {
  u32 index_offset = 0;
  u32 index_count = 0;
  u64 material = 0;  // material asset hash, 0 = default material
  bool blend = false;  // alpha blended: drawn sorted after opaque
  bool water = false;  // routed to the water pipeline
  // lod 0 meshlet range in GpuMesh's meshlet buffers, for the mesh-shader path.
  u32 meshlet_offset = 0;
  u32 meshlet_count = 0;  // 0 = no meshlets built (skinned / mesh shaders off)
};

// An extra level of detail (lods 1+) inside the mesh's concatenated vertex and
// index buffers. The submesh index offsets are absolute in the shared index
// buffer; vertex_offset rebases the indices onto this lod's vertices. Lod 0 is
// GpuMesh::submeshes (vertex_offset 0).
struct GpuLod {
  u32 vertex_offset = 0;
  base::Vector<GpuSubmesh> submeshes;
};

struct GpuMesh {
  GpuBuffer vertices;  // all lods concatenated
  GpuBuffer indices;   // all lods concatenated
  GpuBuffer skinning;      // per-vertex bone indices/weights, set when skinned
  u32 index_count = 0;     // lod 0 index count (shadow / overdraw / blas use lod 0)
  u32 vertex_count = 0;    // lod 0 vertex count
  bool all_blend = false;  // pure transparency (water): stays out of the tlas
  bool no_rt = false;      // grass-like fill geometry, excluded from the tlas
  bool skinned = false;    // drawn with the skinned pipeline against a bone palette
  u32 bindless_index = 0;  // mesh record in the bindless registry
  f32 bounds_center[3] = {0, 0, 0};  // model-space bounding sphere, for gpu culling
  f32 bounds_radius = 0;             // 0 = unknown, treated as never-culled
  base::Vector<GpuSubmesh> submeshes;  // lod 0
  base::Vector<GpuLod> lods;           // extra lods (1+), selected by distance

  // Mesh-shader path (VK_EXT_mesh_shader): lod 0 split into meshlets. Read in the
  // mesh shader via buffer device address, so the *_address fields are what the
  // push constants carry. Empty when mesh shaders are unavailable or the mesh is
  // skinned. meshlets/meshlet_vertices/meshlet_triangles mirror MeshletPass.
  GpuBuffer meshlets;
  GpuBuffer meshlet_vertices;
  GpuBuffer meshlet_triangles;
  u64 meshlets_address = 0;
  u64 meshlet_vertices_address = 0;
  u64 meshlet_triangles_address = 0;
  u64 vertices_address = 0;  // device address of the lod-concatenated vertex buffer
  bool has_meshlets = false;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_RHI_RESOURCES_H_
