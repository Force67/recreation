#ifndef RECREATION_RENDER_PATH_TRACER_H_
#define RECREATION_RENDER_PATH_TRACER_H_

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/resources.h"

namespace rec::render {

class Device;
class RayTracingContext;

// Progressive reference path tracer sharing the realtime TLAS + bindless scene
// tables. Diffuse bounces with sun next-event estimation and the sky cube on
// miss, accumulated into a persistent buffer that the renderer resets whenever
// the camera or lighting moves. Needs ray query; the renderer gates it.
class PathTracer {
 public:
  struct Frame {
    Mat4 inv_view_proj;
    Vec3 camera_pos;
    Vec3 sun_direction;  // travel direction
    f32 sun_intensity = 4.0f;
    Vec3 sun_color{1, 1, 1};
    f32 sun_radius = 0.0f;  // radians, soft sun disk
    u32 frame_index = 0;
    bool reset = false;  // restart accumulation this frame
  };

  bool Initialize(Device& device, VkDescriptorSetLayout bindless_layout);
  void Resize(Device& device, VkExtent2D extent);
  void Destroy(Device& device);

  // Path traces into output (an hdr storage image, usually scene_color),
  // accumulating across frames. Returns the accumulated sample count.
  void AddToGraph(RenderGraph& graph, RayTracingContext& raytracing, u32 tlas_slot,
                  VkDescriptorSet bindless_set, VkImageView sky_view, VkSampler sky_sampler,
                  ResourceHandle output, const Frame& frame);

  u32 accumulated_samples() const { return accumulated_samples_; }
  u32 samples_per_frame() const { return spp_; }

 private:
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  GpuImage accum_;  // rgba32f, persistent; rgb = sum, a = sample count
  VkImageLayout accum_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  VkExtent2D extent_{};
  u32 accumulated_samples_ = 0;
  u32 spp_ = 2;     // samples per dispatch
  u32 bounces_ = 4;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_PATH_TRACER_H_
