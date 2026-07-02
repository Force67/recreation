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
  bool CreateSkyPipeline(BindingLayoutHandle globals_layout, Format color_format,
                         Format motion_format, Format depth_format);

  EnvironmentSystem(const EnvironmentSystem&) = delete;
  EnvironmentSystem& operator=(const EnvironmentSystem&) = delete;

  // Re-renders the sky and both convolutions with internal barriers. Call
  // from the frame command list when the sun changed.
  void RecordUpdate(CommandList& cmd, const Vec3& sun_direction, f32 sun_intensity,
                    const Vec3& sun_color);

  // Fullscreen sky at the far plane; call inside the scene rendering pass.
  void DrawSky(CommandList& cmd, BindingSetHandle globals);

  BindingLayoutHandle env_set_layout() const { return env_set_layout_; }
  TextureView sky_view() const { return sky_.view; }
  TextureView transmittance_view() const { return transmittance_lut_.view; }
  TextureView multiscatter_view() const { return multiscatter_lut_.view; }
  SamplerHandle sampler() const { return sampler_; }
  TextureView prefiltered_view() const { return prefiltered_.view; }
  // Neutral stand-ins for passes that statically bind ddgi inputs.
  TextureView black_array_view() const { return black_array_view_; }
  const GpuBuffer& dummy_volume() const { return dummy_volume_; }
  const GpuBuffer& dummy_storage() const { return dummy_storage_; }
  TextureView shadow_dummy_view() const { return shadow_dummy_.view; }
  SamplerHandle comparison_sampler() const { return shadow_sampler_; }

  struct DdgiBinding {
    TextureView irradiance;
    TextureView distance;
    GpuBuffer volume;
    u64 volume_size = 0;
    // Kept for callers that tracked the atlas layout; the RHI backend derives
    // image layouts from the binding type, so this is no longer consumed.
    ResourceState layout = ResourceState::kShaderReadFragment;
  };

  // Fills a freshly allocated set 2. Null ao view, ddgi binding, shadow view or
  // sun-shadow view fall back to the neutral dummies (white ao, black ddgi, lit
  // cascade shadow, fully-lit sun shadow).
  void WriteEnvSet(BindingSetHandle set, TextureView ao_view, const DdgiBinding* ddgi,
                   TextureView shadow_view = {},
                   const GpuBuffer& cascade_buffer = {}, u64 cascade_size = 0,
                   TextureView opaque_color = {},
                   TextureView sun_shadow_view = {},
                   const GpuBuffer& lights = {}, u64 lights_size = 0,
                   TextureView spec_reflections = {},
                   const GpuBuffer& cluster_counts = {},
                   const GpuBuffer& cluster_indices = {},
                   const GpuBuffer& decal_buffer = {},
                   const GpuBuffer& decal_indices = {},
                   TextureView decal_atlas = {},
                   const GpuBuffer& local_shadow_faces = {},
                   TextureView local_shadow_atlas = {},
                   TextureView decal_normal_atlas = {},
                   TextureView restir_diffuse = {}, TextureView restir_spec = {}) const;

 private:
  explicit EnvironmentSystem(Device& device) : device_(device) {}

  bool CreatePipelines();
  bool CreateImages();
  bool CreateDummies();
  bool BakeBrdfLut();
  bool BakeLuts();  // transmittance + multiple-scattering, baked once

  Device& device_;
  SamplerHandle sampler_;
  SamplerHandle shadow_sampler_;  // comparison sampler for cascades

  GpuImage sky_;         // rgba16f cube
  GpuImage irradiance_;  // rgba16f cube
  GpuImage prefiltered_; // rgba16f cube, kPrefilterMips
  GpuImage brdf_lut_;    // rg16f
  GpuImage transmittance_lut_;  // rgba16f 2d, Hillaire transmittance
  GpuImage multiscatter_lut_;   // rgba16f 2d, Hillaire multiple scattering
  TextureView sky_storage_view_;          // mip 0 storage view
  TextureView irradiance_storage_view_;   // mip 0 storage view
  TextureView prefilter_storage_views_[kPrefilterMips] = {};

  // Neutral fallbacks: white ao, black ddgi atlases, lit shadow, zeroed volume.
  GpuImage white_;
  GpuImage black_array_;
  GpuImage shadow_dummy_;  // 1x1 depth cleared to 1.0 (fully lit)
  GpuImage flat_normal_;   // 1x1 (0.5, 0.5, 1) for the decal channel atlas
  GpuImage black_;         // 1x1 zero, restir-di dummies
  // LTC fit tables for GGX area lights (64x64 RGBA16F, uploaded once).
  GpuImage ltc_matrix_;
  GpuImage ltc_amplitude_;
  TextureView black_array_view_;
  GpuBuffer dummy_volume_;
  GpuBuffer dummy_storage_;  // storage-usage fallback for the SB slots

  PipelineHandle sky_gen_;
  PipelineHandle irradiance_gen_;
  PipelineHandle prefilter_gen_;
  PipelineHandle brdf_gen_;
  PipelineHandle transmittance_gen_;
  PipelineHandle multiscatter_gen_;

  BindingLayoutHandle env_set_layout_;
  PipelineHandle sky_draw_pipeline_;

  bool maps_initialized_ = false;  // first update transitions from kUndefined
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_ENVIRONMENT_H_
