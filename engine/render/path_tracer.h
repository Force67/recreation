#ifndef RECREATION_RENDER_PATH_TRACER_H_
#define RECREATION_RENDER_PATH_TRACER_H_

#include "core/math.h"
#include "render/render_graph.h"
#include "render/rhi/resources.h"

namespace rec::render {

class Device;
class RayTracingContext;

// Reference + playable path tracer, sharing the realtime TLAS + bindless scene
// tables. Diffuse bounces with sun next-event estimation and the sky cube on
// miss. Two modes:
//   - Reference: accumulate into a persistent buffer that resets when the camera
//     or lighting moves, converging to a ground-truth image (AddToGraph).
//   - Denoised: one sample per frame emitting NRD's REBLUR_DIFFUSE inputs
//     (AddGbufferPass), then re-modulate the denoised radiance (AddCompositePass)
//     so the view stays clean while moving.
// Needs ray query; the renderer gates it.
class PathTracer {
 public:
  struct Frame {
    Mat4 inv_view_proj;
    Mat4 view_proj;       // denoised: viewZ; unjittered
    Mat4 prev_view_proj;  // denoised: camera-motion vectors; unjittered
    Vec3 camera_pos;
    Vec3 sun_direction;  // travel direction
    f32 sun_intensity = 4.0f;
    Vec3 sun_color{1, 1, 1};
    f32 sun_radius = 0.0f;  // radians, soft sun disk
    u32 frame_index = 0;
    bool reset = false;  // reference: restart accumulation this frame
  };

  // NRD guide targets the denoised path writes (created by the renderer as
  // transients) and the composite reads back.
  struct GbufferTargets {
    ResourceHandle radiance_hitdist = kInvalidResource;
    ResourceHandle normal_roughness = kInvalidResource;
    ResourceHandle viewz = kInvalidResource;
    ResourceHandle motion = kInvalidResource;
    ResourceHandle albedo = kInvalidResource;
    ResourceHandle background = kInvalidResource;
  };

  bool Initialize(Device& device, VkDescriptorSetLayout bindless_layout);
  void Resize(Device& device, VkExtent2D extent);
  void Destroy(Device& device);

  // Reference accumulation into output (an hdr storage image, usually
  // scene_color), accumulating across frames.
  void AddToGraph(RenderGraph& graph, RayTracingContext& raytracing, u32 tlas_slot,
                  VkDescriptorSet bindless_set, VkImageView sky_view, VkSampler sky_sampler,
                  ResourceHandle output, const Frame& frame);

  // Denoised: trace one sample and write the NRD REBLUR_DIFFUSE inputs into t.
  void AddGbufferPass(RenderGraph& graph, RayTracingContext& raytracing, u32 tlas_slot,
                      VkDescriptorSet bindless_set, VkImageView sky_view, VkSampler sky_sampler,
                      const GbufferTargets& t, const Frame& frame);

  // Denoised: scene_color = denoised_radiance * albedo + background.
  void AddCompositePass(RenderGraph& graph, ResourceHandle denoised, ResourceHandle albedo,
                        ResourceHandle background, ResourceHandle output);

  u32 accumulated_samples() const { return accumulated_samples_; }
  u32 samples_per_frame() const { return spp_; }

 private:
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  // Denoised path (NRD inputs + composite).
  VkDescriptorSetLayout gbuffer_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout gbuffer_layout_ = VK_NULL_HANDLE;
  VkPipeline gbuffer_pipeline_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout composite_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout composite_layout_ = VK_NULL_HANDLE;
  VkPipeline composite_pipeline_ = VK_NULL_HANDLE;
  GpuImage accum_;  // rgba32f, persistent; rgb = sum, a = sample count
  VkImageLayout accum_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  VkExtent2D extent_{};
  u32 accumulated_samples_ = 0;
  u32 spp_ = 2;     // samples per dispatch
  u32 bounces_ = 4;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_PATH_TRACER_H_
