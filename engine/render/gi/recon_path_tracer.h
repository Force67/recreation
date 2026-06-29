#ifndef RECREATION_RENDER_RECON_PATH_TRACER_H_
#define RECREATION_RENDER_RECON_PATH_TRACER_H_

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/resources.h"

namespace rec::render {

class Device;
class RayTracingContext;

// SVGF-style reconstruction path tracer (the "gameplay" mode), separate from the
// brute-force reference and the NRD-denoised path. Traces one noisy sample per
// pixel into a g-buffer + demodulated diffuse irradiance, then reconstructs with
// its own temporal accumulation (motion reproject + history rejection + clamp +
// moments/variance) and an a-trous wavelet filter, and composites albedo back in.
// Fully in-tree and tunable, with debug views. Needs ray query.
class ReconPathTracer {
 public:
  struct Frame {
    Mat4 inv_view_proj;
    Mat4 view_proj;
    Mat4 prev_view_proj;
    Vec3 camera_pos;
    Vec3 sun_direction;  // travel direction
    f32 sun_intensity = 4.0f;
    Vec3 sun_color{1, 1, 1};
    f32 sun_radius = 0.0f;
    f32 pixel_spread = 0.0f;
    u32 spp = 1;
    u32 frame_index = 0;
    bool reset = false;
    // Dynamic point lights (torches/lanterns/spells), sampled by NEE in the
    // gbuffer pass. The buffer is the renderer's frame.lights; count is capped.
    VkBuffer lights = VK_NULL_HANDLE;
    VkDeviceSize lights_size = 0;
    u32 light_count = 0;
    // Tunables.
    f32 current_weight_min = 0.05f;  // floor on current-frame weight (responsiveness)
    u32 max_history = 32;            // history length cap (frames)
    u32 atrous_passes = 4;           // a-trous iterations
    u32 debug_mode = 0;              // 0 final, 1 lighting, 2 history, 3 variance, 4 motion, 5 normal, 6 albedo
  };

  bool Initialize(Device& device, VkDescriptorSetLayout bindless_layout);
  void Resize(Device& device, VkExtent2D extent);
  void Destroy(Device& device);

  // Reconstructs the path-traced image into output (scene_color, an hdr storage
  // image), in place of the raster path.
  void AddToGraph(RenderGraph& graph, RayTracingContext& raytracing, u32 tlas_slot,
                  VkDescriptorSet bindless_set, VkImageView sky_view, VkSampler sky_sampler,
                  ResourceHandle output, const Frame& frame);

 private:
  struct PingPong {
    GpuImage image[2];
    VkImageLayout layout[2] = {VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED};
  };

  bool CreatePipelines(Device& device, VkDescriptorSetLayout bindless_layout);
  void CreateBuffers(Device& device, VkExtent2D extent);
  void DestroyBuffers(Device& device);

  // Reusable per-signal reconstruction (diffuse irradiance and specular both run
  // through these; they share the gbuffer history nr/viewz/matid + motion).
  void RunTemporal(RenderGraph& graph, ResourceHandle noisy, ResourceHandle ac_c,
                   ResourceHandle ac_p, ResourceHandle mo_c, ResourceHandle mo_p, ResourceHandle nr_c,
                   ResourceHandle nr_p, ResourceHandle vz_c, ResourceHandle vz_p, ResourceHandle id_c,
                   ResourceHandle id_p, ResourceHandle motion, const Frame& frame);
  ResourceHandle RunAtrous(RenderGraph& graph, ResourceHandle in, ResourceHandle ping,
                           ResourceHandle pong, ResourceHandle nr_c, ResourceHandle vz_c,
                           ResourceHandle mo_c, u32 passes, bool spec);

  Device* device_ = nullptr;
  VkExtent2D extent_{};
  u32 spp_ = 1;
  u32 bounces_ = 2;

  // gbuffer (set 0: 7 storage + tlas + sky; set 1: bindless)
  VkDescriptorSetLayout gbuffer_set_ = VK_NULL_HANDLE;
  VkPipelineLayout gbuffer_layout_ = VK_NULL_HANDLE;
  VkPipeline gbuffer_pipeline_ = VK_NULL_HANDLE;
  // temporal
  VkDescriptorSetLayout temporal_set_ = VK_NULL_HANDLE;
  VkPipelineLayout temporal_layout_ = VK_NULL_HANDLE;
  VkPipeline temporal_pipeline_ = VK_NULL_HANDLE;
  // atrous
  VkDescriptorSetLayout atrous_set_ = VK_NULL_HANDLE;
  VkPipelineLayout atrous_layout_ = VK_NULL_HANDLE;
  VkPipeline atrous_pipeline_ = VK_NULL_HANDLE;
  // composite
  VkDescriptorSetLayout composite_set_ = VK_NULL_HANDLE;
  VkPipelineLayout composite_layout_ = VK_NULL_HANDLE;
  VkPipeline composite_pipeline_ = VK_NULL_HANDLE;

  // Cross-frame ping-pong buffers (indexed by frame_index & 1).
  PingPong accum_;        // rgba16f accumulated diffuse irradiance + variance
  PingPong moments_;      // rgba16f mean, meanSq, variance, historyLen
  PingPong spec_accum_;   // rgba16f accumulated specular + variance
  PingPong spec_moments_; // rgba16f specular moments
  PingPong normal_rough_; // rgba16f normal*0.5+0.5, roughness
  PingPong viewz_;        // r32f
  PingPong matid_;        // r32ui
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_RECON_PATH_TRACER_H_
