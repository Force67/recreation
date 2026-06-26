#ifndef RECREATION_RENDER_MESHLET_H_
#define RECREATION_RENDER_MESHLET_H_

#include "asset/mesh.h"
#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/resources.h"

namespace rec::render {

class Device;

// Mirrors the Meshlet struct in meshlet.ms / mesh_scene.ms (std430, 48 bytes).
struct Meshlet {
  f32 center_radius[4];  // xyz center, w radius (model space)
  f32 cone[4];           // xyz axis, w cutoff (>=2 disables cone cull)
  u32 vertex_offset = 0;
  u32 triangle_offset = 0;
  u32 vertex_count = 0;
  u32 triangle_count = 0;
};

struct MeshletGeometry {
  base::Vector<Meshlet> meshlets;
  base::Vector<u32> vertex_indices;  // index into the source vertex array
  base::Vector<u32> triangles;       // 3 local indices packed per u32
};

// Greedy spatial clustering of a triangle range into meshlets (<=64 verts /
// <=124 tris), with a bounding sphere and backface normal cone per cluster for
// gpu cluster culling. vertex_indices are absolute indices into `vertices`.
MeshletGeometry BuildMeshletGeometry(const asset::Vertex* vertices, u32 vertex_count,
                                     const u32* indices, u32 index_count);

// Mesh-shader meshlet path (VK_EXT_mesh_shader). A mesh is split into meshlets
// (clusters of <=64 verts / <=124 tris) at upload; a mesh shader dispatches one
// workgroup per meshlet, frustum- and backface-cone-culls the cluster on the
// gpu, and emits the surviving meshlet's geometry with no vertex/index fetch.
// Each meshlet is tinted a distinct color so the decomposition and the cluster
// culling are directly visible. Used by the --demo meshlet scene.
class MeshletPass {
 public:
  struct Vertex {
    f32 px, py, pz;
    f32 nx, ny, nz;
  };

  bool Initialize(Device& device, VkFormat color_format, VkFormat depth_format);
  void Destroy(Device& device);

  // Builds meshlets from the mesh's lod 0 and uploads the gpu buffers. Replaces
  // any previously uploaded mesh.
  void Upload(Device& device, const asset::Mesh& mesh);
  bool active() const { return meshlet_count_ > 0; }
  u32 meshlet_count() const { return meshlet_count_; }
  // Survivors of last frame's cluster cull (one frame stale, fence-safe).
  u32 last_visible(u32 slot) const;

  // Draws the uploaded meshlets into color (depth tested/written), culling each
  // cluster against the frustum planes and the camera in the mesh shader.
  void AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                  const Mat4& view_proj, const f32 planes[5][4], const Vec3& camera, u32 slot);

 private:
  static constexpr u32 kFramesInFlight = 2;
  bool available_ = false;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;

  GpuBuffer meshlets_;
  GpuBuffer meshlet_vertices_;
  GpuBuffer meshlet_triangles_;
  GpuBuffer vertices_;
  GpuBuffer counters_[kFramesInFlight];  // host-visible visible-meshlet counter
  u32 meshlet_count_ = 0;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_MESHLET_H_
