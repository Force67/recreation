#ifndef RECREATION_RENDER_DDGI_H_
#define RECREATION_RENDER_DDGI_H_

#include <memory>

#include "core/math.h"
#include "render/bindless.h"
#include "render/environment.h"
#include "render/render_graph.h"
#include "render/rhi/device.h"

namespace rec::render {

class RayTracingContext;

// Dynamic diffuse global illumination with irradiance probes (the rtxgi /
// DDGI scheme): a camera following probe grid traces a rotated fibonacci
// sphere of rays per probe each frame, blends the results into octahedral
// irradiance and filtered distance atlases, and the forward pass samples
// them with chebyshev visibility weights.
//
// Probe ray hits fetch their triangle's interpolated normal, uv and
// material through the bindless scene tables, then shade from the sun
// (shadow tested), emissive and the previous frame's probes, so the bounce
// carries real material color.
class DdgiSystem {
 public:
  static constexpr u32 kProbesX = 16;
  static constexpr u32 kProbesY = 8;
  static constexpr u32 kProbesZ = 16;
  static constexpr u32 kIrradianceTexels = 6;
  static constexpr u32 kDistanceTexels = 14;
  static constexpr u32 kRaysPerProbe = 96;

  struct Settings {
    f32 probe_spacing = 1.5f;  // meters
    f32 hysteresis = 0.97f;
    f32 energy_scale = 1.0f;
  };

  static std::unique_ptr<DdgiSystem> Create(Device& device, VkImageView sky_view,
                                            VkSampler sky_sampler, BindlessRegistry& bindless);
  ~DdgiSystem();

  DdgiSystem(const DdgiSystem&) = delete;
  DdgiSystem& operator=(const DdgiSystem&) = delete;

  void Configure(const Settings& settings);

  // Re-centers the volume on the camera (snapped to the probe grid; the
  // hysteresis re-converges probes after a snap) and adds the ray trace,
  // blend and border passes.
  void AddToGraph(RenderGraph& graph, RayTracingContext& raytracing, u32 tlas_slot,
                  const Vec3& camera, const Vec3& sun_direction, f32 sun_intensity,
                  const Vec3& sun_color, u32 frame_index);

  // Bound as part of descriptor set 2 by the renderer.
  EnvironmentSystem::DdgiBinding binding(u32 frame_index) const;

 private:
  struct VolumeData {
    f32 origin[4];
    u32 counts[4];
    f32 params[4];
  };

  explicit DdgiSystem(Device& device) : device_(device) {}

  bool CreateResources(VkImageView sky_view, VkSampler sky_sampler);
  bool CreatePipelines();

  Device& device_;
  Settings settings_;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;

  GpuImage irradiance_;  // rgba16f atlas, 2d array view bound to shading
  GpuImage distance_;    // rg16f moments atlas
  GpuImage rays_;        // rgba16f: radiance + hit distance, ray x probe
  VkImageView irradiance_array_view_ = VK_NULL_HANDLE;
  VkImageView distance_array_view_ = VK_NULL_HANDLE;
  GpuBuffer volume_buffers_[2];  // host visible, ping pong by frame parity
  bool atlas_initialized_ = false;
  bool history_valid_ = false;
  Vec3 origin_{};

  VkImageView sky_view_ = VK_NULL_HANDLE;
  VkSampler sky_sampler_ = VK_NULL_HANDLE;
  BindlessRegistry* bindless_ = nullptr;

  VkDescriptorSetLayout rays_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout rays_layout_ = VK_NULL_HANDLE;
  VkPipeline rays_pipeline_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout blend_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout blend_layout_ = VK_NULL_HANDLE;
  VkPipeline blend_pipeline_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout border_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout border_layout_ = VK_NULL_HANDLE;
  VkPipeline border_pipeline_ = VK_NULL_HANDLE;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_DDGI_H_
