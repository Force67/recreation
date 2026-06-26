#ifndef RECREATION_RENDER_AMBIENT_OCCLUSION_H_
#define RECREATION_RENDER_AMBIENT_OCCLUSION_H_

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/resources.h"

namespace rec::render {

class Device;
class RayTracingContext;

// Ray traced ambient occlusion: cosine hemisphere rays through the frame TLAS,
// producing a raw normalized hit distance packed for NRD's REBLUR occlusion
// denoiser (which owns all temporal/spatial filtering). Needs ray query
// support; the renderer skips it otherwise.
class RtaoPass {
 public:
  struct Settings {
    f32 radius = 1.2f;  // meters
    u32 ray_count = 2;
  };

  bool Initialize(Device& device);
  void Resize(Device& device, VkExtent2D extent) { extent_ = extent; }
  void Destroy(Device& device);

  void Configure(const Settings& settings) { settings_ = settings; }

  // Adds the trace pass and returns the packed normalized hit distance (R8).
  ResourceHandle AddToGraph(RenderGraph& graph, RayTracingContext& raytracing, u32 tlas_slot,
                            ResourceHandle depth, ResourceHandle normals,
                            const Mat4& inv_view_proj, u32 frame_index, f32 near_plane,
                            const f32 hit_dist_params[3]);

  static constexpr VkFormat kHitDistFormat = VK_FORMAT_R8_UNORM;

 private:
  Settings settings_;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkExtent2D extent_{};
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_AMBIENT_OCCLUSION_H_
