#ifndef RECREATION_RENDER_SSAO_H_
#define RECREATION_RENDER_SSAO_H_

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/resources.h"

namespace rec::render {

class Device;

// Screen-space ambient occlusion: a hemisphere-kernel pass over the prepass
// depth + world normals, no ray tracing required. This is the fallback AO for
// devices without ray query (mobile, low-end, forced low preset). Output is a
// single-channel occlusion map (1 = unoccluded) shaped like RtaoPass so it
// drops straight into the same env-set ao slot. TAA cleans up the residual
// noise, so there is no separate denoiser here.
class SsaoPass {
 public:
  struct Settings {
    f32 radius = 0.6f;       // meters
    f32 intensity = 1.8f;    // occlusion strength
    f32 power = 1.5f;        // contrast curve on the final ao
    u32 sample_count = 16;   // hemisphere taps per pixel
  };

  bool Initialize(Device& device);
  void Resize(Device& device, VkExtent2D extent) { extent_ = extent; }
  void Destroy(Device& device);

  void Configure(const Settings& settings) { settings_ = settings; }

  // proj_scale is {proj.m[0], proj.m[5]}, mapping a world radius to ndc.
  ResourceHandle AddToGraph(RenderGraph& graph, ResourceHandle depth, ResourceHandle normals,
                            const Mat4& inv_view_proj, const f32 proj_scale[2], f32 near_plane,
                            u32 frame_index);

  static constexpr VkFormat kAoFormat = VK_FORMAT_R8_UNORM;

 private:
  Settings settings_;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkExtent2D extent_{};
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_SSAO_H_
