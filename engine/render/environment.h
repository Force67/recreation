#ifndef RECREATION_RENDER_ENVIRONMENT_H_
#define RECREATION_RENDER_ENVIRONMENT_H_

#include <memory>

#include "core/math.h"
#include "render/rhi/device.h"

namespace rec::render {

// Image based lighting around a procedural atmosphere. Owns the sky cubemap
// plus its diffuse irradiance and ggx prefiltered convolutions, the split
// sum brdf lut, and the sky background pipeline. The cubemaps regenerate in
// frame whenever the sun changes; the lut bakes once at startup.
//
// Also owns the layout of descriptor set 2 of the mesh pipeline (ibl inputs,
// the per frame ao texture and the ddgi probe volume) and neutral fallbacks
// for whatever feature is disabled.
class EnvironmentSystem {
 public:
  static constexpr u32 kSkySize = 128;
  static constexpr u32 kIrradianceSize = 32;
  static constexpr u32 kPrefilterSize = 128;
  static constexpr u32 kPrefilterMips = 6;  // matches kPrefilterMips in mesh.ps
  static constexpr u32 kBrdfLutSize = 512;
  // Hillaire 2020 atmosphere LUTs (sun-independent, baked once).
  static constexpr u32 kTransmittanceW = 256;
  static constexpr u32 kTransmittanceH = 64;
  static constexpr u32 kMultiScatterSize = 32;

  static std::unique_ptr<EnvironmentSystem> Create(Device& device);
  ~EnvironmentSystem();

  // Builds the sky background pipeline; needs the mesh pipeline's set 0
  // layout and the scene pass attachment formats, so it runs after the mesh
  // pipeline exists.
  bool CreateSkyPipeline(VkDescriptorSetLayout globals_layout, VkFormat color_format,
                         VkFormat motion_format, VkFormat depth_format);

  EnvironmentSystem(const EnvironmentSystem&) = delete;
  EnvironmentSystem& operator=(const EnvironmentSystem&) = delete;

  // Re-renders the sky and both convolutions with internal barriers. Call
  // from the frame command buffer when the sun changed.
  void RecordUpdate(VkCommandBuffer cmd, const Vec3& sun_direction, f32 sun_intensity,
                    const Vec3& sun_color);

  // Fullscreen sky at the far plane; call inside the scene rendering pass.
  void DrawSky(VkCommandBuffer cmd, VkDescriptorSet globals);

  VkDescriptorSetLayout env_set_layout() const { return env_set_layout_; }
  VkImageView sky_view() const { return sky_.view; }
  VkSampler sampler() const { return sampler_; }

  struct DdgiBinding {
    VkImageView irradiance = VK_NULL_HANDLE;
    VkImageView distance = VK_NULL_HANDLE;
    VkBuffer volume = VK_NULL_HANDLE;
    u64 volume_size = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  };

  // Fills a freshly allocated set 2. Null ao view, ddgi binding, shadow view or
  // sun-shadow view fall back to the neutral dummies (white ao, black ddgi, lit
  // cascade shadow, fully-lit sun shadow).
  void WriteEnvSet(VkDescriptorSet set, VkImageView ao_view, const DdgiBinding* ddgi,
                   VkImageView shadow_view = VK_NULL_HANDLE,
                   VkBuffer cascade_buffer = VK_NULL_HANDLE, u64 cascade_size = 0,
                   VkImageView opaque_color = VK_NULL_HANDLE,
                   VkImageView sun_shadow_view = VK_NULL_HANDLE,
                   VkBuffer lights = VK_NULL_HANDLE, u64 lights_size = 0) const;

 private:
  struct ComputePass {
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;  // static inputs, written once
  };

  explicit EnvironmentSystem(Device& device) : device_(device) {}

  bool CreatePipelines();
  bool CreateImages();
  bool CreateDummies();
  bool BakeBrdfLut();
  bool BakeLuts();  // transmittance + multiple-scattering, baked once
  void DestroyComputePass(ComputePass& pass);

  Device& device_;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkSampler shadow_sampler_ = VK_NULL_HANDLE;  // comparison sampler for cascades
  VkDescriptorPool pool_ = VK_NULL_HANDLE;

  GpuImage sky_;         // rgba16f cube
  GpuImage irradiance_;  // rgba16f cube
  GpuImage prefiltered_; // rgba16f cube, kPrefilterMips
  GpuImage brdf_lut_;    // rg16f
  GpuImage transmittance_lut_;  // rgba16f 2d, Hillaire transmittance
  GpuImage multiscatter_lut_;   // rgba16f 2d, Hillaire multiple scattering
  VkImageView sky_storage_view_ = VK_NULL_HANDLE;          // 2d array, mip 0
  VkImageView irradiance_storage_view_ = VK_NULL_HANDLE;   // 2d array, mip 0
  VkImageView prefilter_storage_views_[kPrefilterMips] = {};

  // Neutral fallbacks: white ao, black ddgi atlases, lit shadow, zeroed volume.
  GpuImage white_;
  GpuImage black_array_;
  GpuImage shadow_dummy_;  // 1x1 depth cleared to 1.0 (fully lit)
  VkImageView black_array_view_ = VK_NULL_HANDLE;
  GpuBuffer dummy_volume_;

  ComputePass sky_gen_;
  ComputePass irradiance_gen_;
  ComputePass prefilter_gen_;
  VkDescriptorSet prefilter_sets_[kPrefilterMips] = {};
  ComputePass brdf_gen_;
  ComputePass transmittance_gen_;
  ComputePass multiscatter_gen_;

  VkDescriptorSetLayout env_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout sky_draw_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout sky_draw_layout_ = VK_NULL_HANDLE;
  VkPipeline sky_draw_pipeline_ = VK_NULL_HANDLE;
  VkDescriptorSet sky_draw_set_ = VK_NULL_HANDLE;

  bool maps_initialized_ = false;  // first update transitions from UNDEFINED
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_ENVIRONMENT_H_
