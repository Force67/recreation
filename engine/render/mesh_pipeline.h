#ifndef RECREATION_RENDER_MESH_PIPELINE_H_
#define RECREATION_RENDER_MESH_PIPELINE_H_

#include <memory>

#include "core/math.h"
#include "render/rhi/device.h"

namespace rec::render {

// Per frame camera and lighting state, bound as set 0. Layout matches the
// std140 block in mesh.vert/mesh.frag.
struct FrameGlobals {
  Mat4 view_proj;
  Mat4 prev_view_proj;
  f32 jitter[2] = {0, 0};  // ndc units
  f32 prev_jitter[2] = {0, 0};
  f32 sun_direction[4] = {0, -1, 0, 4};  // xyz travel direction, w intensity
  f32 sun_color[4] = {1, 1, 1, 0.06f};   // rgb color, w ambient
  f32 camera_position[4] = {0, 0, 0, 0};
};

// Stays within the 128 byte push constant minimum, everything else goes
// through the globals buffer.
struct MeshPushConstants {
  Mat4 model;
  Mat4 prev_model;
};

// Forward pbr pipeline: classic vertex buffer, metallic roughness shading,
// reversed z depth. Outputs hdr color and motion vectors. Set 0 is the frame
// globals (plus the TLAS when raytracing is available), set 1 the material.
// Variants cover ray queried shadows on/off and a wireframe debug fill mode,
// all sharing one layout so they swap mid-frame without rebinding sets.
class MeshPipeline {
 public:
  static std::unique_ptr<MeshPipeline> Create(Device& device, VkFormat color_format,
                                              VkFormat motion_format, VkFormat depth_format,
                                              VkDescriptorSetLayout material_layout);
  ~MeshPipeline();

  MeshPipeline(const MeshPipeline&) = delete;
  MeshPipeline& operator=(const MeshPipeline&) = delete;

  VkDescriptorSetLayout set_layout() const { return set_layout_; }
  bool has_rt_variant() const { return pipelines_[kRt] != VK_NULL_HANDLE; }

  void Bind(VkCommandBuffer cmd, VkDescriptorSet globals, bool rt_shadows, bool wireframe);
  void BindMaterial(VkCommandBuffer cmd, VkDescriptorSet material);
  void Draw(VkCommandBuffer cmd, const GpuMesh& mesh, const MeshPushConstants& push);
  void DrawSubmesh(VkCommandBuffer cmd, const GpuSubmesh& submesh);

 private:
  // Variant index bits.
  static constexpr u32 kRt = 1;
  static constexpr u32 kWire = 2;

  explicit MeshPipeline(Device& device) : device_(device) {}

  Device& device_;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipelines_[4] = {};  // [rt | wire]
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_MESH_PIPELINE_H_
