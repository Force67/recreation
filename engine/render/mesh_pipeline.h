#ifndef RECREATION_RENDER_MESH_PIPELINE_H_
#define RECREATION_RENDER_MESH_PIPELINE_H_

#include <memory>

#include "core/math.h"
#include "render/rhi/device.h"

namespace rec::render {

// Per frame camera state, bound as set 0. Layout matches std140.
struct FrameGlobals {
  Mat4 view_proj;
  Mat4 prev_view_proj;
  f32 jitter[2] = {0, 0};       // ndc units
  f32 prev_jitter[2] = {0, 0};
};

// Stays within the 128 byte push constant minimum, everything else goes
// through the globals buffer.
struct MeshPushConstants {
  Mat4 model;
  Mat4 prev_model;
};

// Bringup pipeline: classic vertex buffer, lambert shading, reversed z
// depth. Outputs hdr color and motion vectors. The real material system
// replaces this once descriptors and bindless textures land.
class MeshPipeline {
 public:
  static std::unique_ptr<MeshPipeline> Create(Device& device, VkFormat color_format,
                                              VkFormat motion_format, VkFormat depth_format);
  ~MeshPipeline();

  MeshPipeline(const MeshPipeline&) = delete;
  MeshPipeline& operator=(const MeshPipeline&) = delete;

  VkDescriptorSetLayout set_layout() const { return set_layout_; }

  void Bind(VkCommandBuffer cmd, VkDescriptorSet globals);
  void Draw(VkCommandBuffer cmd, const GpuMesh& mesh, const MeshPushConstants& push);

 private:
  explicit MeshPipeline(Device& device) : device_(device) {}

  Device& device_;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_MESH_PIPELINE_H_
