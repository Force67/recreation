#ifndef RECREATION_RENDER_WATER_H_
#define RECREATION_RENDER_WATER_H_

#include <memory>

#include "render/core/bindless.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rec::render {

// Water surface pipeline: fbm wave normals, raytraced reflections shaded
// through the bindless tables, screen space refraction with absorption
// against a snapshot of the opaque pass. Needs ray query; without it water
// falls back to the generic blend pipeline upstream.
class WaterPass {
 public:
  // Set layouts mirror the water.ps bindings: 0 mesh globals (+tlas),
  // 1 material, 2 environment, 3 bindless, 4 the opaque snapshot.
  static std::unique_ptr<WaterPass> Create(Device& device, VkFormat color_format,
                                           VkFormat motion_format, VkFormat depth_format,
                                           VkDescriptorSetLayout globals_layout,
                                           VkDescriptorSetLayout material_layout,
                                           VkDescriptorSetLayout environment_layout,
                                           VkDescriptorSetLayout bindless_layout);
  ~WaterPass();

  WaterPass(const WaterPass&) = delete;
  WaterPass& operator=(const WaterPass&) = delete;

  // Compute copy of the opaque scene color into the snapshot transient.
  void RecordCopy(PassContext& ctx, ResourceHandle scene_color, ResourceHandle opaque_color,
                  u32 width, u32 height);

  // Binds the water pipeline; sets 0-2 are the frame's globals/material-less
  // environment sets, set 4 is written from the snapshot views.
  void Bind(PassContext& ctx, VkDescriptorSet globals, VkDescriptorSet environment,
            VkDescriptorSet bindless, ResourceHandle opaque_color, ResourceHandle opaque_depth);
  void BindMaterial(VkCommandBuffer cmd, VkDescriptorSet material);
  VkPipelineLayout layout() const { return layout_; }

 private:
  explicit WaterPass(Device& device) : device_(device) {}

  Device& device_;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout input_set_layout_ = VK_NULL_HANDLE;  // set 4
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout copy_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout copy_layout_ = VK_NULL_HANDLE;
  VkPipeline copy_pipeline_ = VK_NULL_HANDLE;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_WATER_H_
