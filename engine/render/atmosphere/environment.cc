#include "render/atmosphere/environment.h"

#include <cstring>
#include <utility>

#include "core/log.h"
#include "shaders/brdf_lut_cs_hlsl.h"
#include "shaders/fullscreen_vs_hlsl.h"
#include "shaders/irradiance_cs_hlsl.h"
#include "shaders/multiscatter_lut_cs_hlsl.h"
#include "shaders/prefilter_cs_hlsl.h"
#include "shaders/sky_cs_hlsl.h"
#include "shaders/sky_ps_hlsl.h"
#include "shaders/transmittance_lut_cs_hlsl.h"

namespace rec::render {
namespace {

struct SkyPush {
  f32 sun_direction[3];
  f32 intensity;
  f32 sun_color[3];
  f32 face_size;
};

struct SizePush {
  f32 size;
};

struct PrefilterPush {
  f32 face_size;
  f32 roughness;
};

struct LutPush {
  f32 size[2];  // (width, height) in texels
};

}  // namespace

std::unique_ptr<EnvironmentSystem> EnvironmentSystem::Create(Device& device) {
  auto env = std::unique_ptr<EnvironmentSystem>(new EnvironmentSystem(device));

  env->sampler_ = device.GetSampler({.min_filter = Filter::kLinear,
                                     .mag_filter = Filter::kLinear,
                                     .mip_filter = Filter::kLinear,
                                     .address_u = AddressMode::kClampToEdge,
                                     .address_v = AddressMode::kClampToEdge,
                                     .address_w = AddressMode::kClampToEdge});
  if (!env->sampler_) return nullptr;

  // Comparison sampler for the cascade shadow atlas: hardware pcf, depth-less-or
  // -equal returns the lit fraction, clamped so taps near a cascade edge hold.
  env->shadow_sampler_ = device.GetSampler({.min_filter = Filter::kLinear,
                                            .mag_filter = Filter::kLinear,
                                            .mip_filter = Filter::kNearest,
                                            .address_u = AddressMode::kClampToEdge,
                                            .address_v = AddressMode::kClampToEdge,
                                            .address_w = AddressMode::kClampToEdge,
                                            .max_lod = 0.0f,
                                            .compare_enable = true,
                                            .compare_op = CompareOp::kLessEqual});
  if (!env->shadow_sampler_) return nullptr;

  if (!env->CreateImages() || !env->CreateDummies()) return nullptr;
  if (!env->CreatePipelines()) return nullptr;
  if (!env->BakeLuts()) return nullptr;  // transmittance + multiscatter, before any sky update
  if (!env->BakeBrdfLut()) return nullptr;
  return env;
}

bool EnvironmentSystem::CreateImages() {
  TextureUsageFlags usage = kTextureUsageSampled | kTextureUsageStorage;
  sky_ = device_.CreateImageCube(Format::kRGBA16Float, kSkySize, usage);
  irradiance_ = device_.CreateImageCube(Format::kRGBA16Float, kIrradianceSize, usage);
  prefiltered_ = device_.CreateImageCube(Format::kRGBA16Float, kPrefilterSize, usage,
                                         kPrefilterMips);
  brdf_lut_ = device_.CreateImage2D(Format::kRG16Float, {kBrdfLutSize, kBrdfLutSize}, usage);
  // Atmosphere LUTs: a single 2d view serves both the storage write (kGeneral)
  // and the later sampled reads, like the brdf lut.
  transmittance_lut_ = device_.CreateImage2D(Format::kRGBA16Float,
                                             {kTransmittanceW, kTransmittanceH}, usage);
  multiscatter_lut_ = device_.CreateImage2D(Format::kRGBA16Float,
                                            {kMultiScatterSize, kMultiScatterSize}, usage);
  if (!sky_ || !irradiance_ || !prefiltered_ || !brdf_lut_ || !transmittance_lut_ ||
      !multiscatter_lut_)
    return false;

  sky_storage_view_ = device_.CreateMipView(sky_, 0);
  if (!sky_storage_view_) return false;
  irradiance_storage_view_ = device_.CreateMipView(irradiance_, 0);
  if (!irradiance_storage_view_) return false;
  for (u32 mip = 0; mip < kPrefilterMips; ++mip) {
    prefilter_storage_views_[mip] = device_.CreateMipView(prefiltered_, mip);
    if (!prefilter_storage_views_[mip]) return false;
  }
  return true;
}

bool EnvironmentSystem::CreateDummies() {
  white_ = device_.CreateImage2D(Format::kR8Unorm, {1, 1},
                                 kTextureUsageSampled | kTextureUsageTransferDst);
  black_array_ = device_.CreateImage2D(Format::kRGBA16Float, {1, 1},
                                       kTextureUsageSampled | kTextureUsageTransferDst);
  // Stand-in shadow atlas (1x1 depth, cleared lit) so the env set is always
  // complete even when cascaded shadow maps are off.
  shadow_dummy_ = device_.CreateImage2D(Format::kD32Float, {1, 1},
                                        kTextureUsageSampled | kTextureUsageTransferDst);
  if (!white_ || !black_array_ || !shadow_dummy_) return false;

  // The shaders declare Texture2DArray for the ddgi slots; the dummy must be
  // an array view so the descriptor's view type matches when ddgi is off.
  black_array_view_ = device_.CreateArrayView(black_array_);
  if (!black_array_view_) return false;

  dummy_volume_ = device_.CreateBuffer(512, kBufferUsageUniform, true);
  dummy_storage_ = device_.CreateBuffer(512, kBufferUsageStorage, true);
  if (!dummy_volume_.mapped) return false;
  std::memset(dummy_volume_.mapped, 0, 512);

  device_.ImmediateSubmit([&](CommandList& cmd) {
    for (GpuImage* image : {&white_, &black_array_}) {
      cmd.Barrier(Transition(*image, ResourceState::kUndefined, ResourceState::kCopyDst));
      f32 clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      if (image == &white_) clear[0] = 1.0f;
      cmd.ClearColor(*image, clear);
      cmd.Barrier(Transition(*image, ResourceState::kCopyDst,
                             ResourceState::kShaderReadFragment));
    }

    // The depth dummy: clear to 1.0 (fully lit) and leave it shader-readable.
    cmd.Barrier(Transition(shadow_dummy_, ResourceState::kUndefined, ResourceState::kCopyDst));
    cmd.ClearDepth(shadow_dummy_, 1.0f);
    cmd.Barrier(Transition(shadow_dummy_, ResourceState::kCopyDst,
                           ResourceState::kShaderReadFragment));
  });
  return true;
}

bool EnvironmentSystem::CreatePipelines() {
  // sampled_count: number of combined-image-sampler inputs after the storage
  // image at binding 0 (the sky pass takes two: the transmittance + multi-
  // scattering LUTs).
  auto make_compute = [&](PipelineHandle* pipeline, ShaderBlob shader, u32 sampled_count,
                          u32 push_size, const char* name) {
    PipelineBindings set;
    set.slots.push_back({0, BindingType::kStorageImage});
    for (u32 i = 1; i <= sampled_count; ++i) {
      set.slots.push_back({i, BindingType::kCombinedTextureSampler});
    }
    ComputePipelineDesc desc;
    desc.shader = shader;
    desc.sets.push_back(std::move(set));
    desc.push_constant_size = push_size;
    desc.debug_name = name;
    *pipeline = device_.CreateComputePipeline(desc);
    return static_cast<bool>(*pipeline);
  };

  if (!make_compute(&sky_gen_, REC_SHADER(k_sky_cs_hlsl), 2, sizeof(SkyPush), "sky_gen") ||
      !make_compute(&irradiance_gen_, REC_SHADER(k_irradiance_cs_hlsl), 1, sizeof(SizePush),
                    "irradiance_gen") ||
      !make_compute(&prefilter_gen_, REC_SHADER(k_prefilter_cs_hlsl), 1, sizeof(PrefilterPush),
                    "prefilter_gen") ||
      !make_compute(&brdf_gen_, REC_SHADER(k_brdf_lut_cs_hlsl), 0, sizeof(SizePush),
                    "brdf_gen") ||
      !make_compute(&transmittance_gen_, REC_SHADER(k_transmittance_lut_cs_hlsl), 0,
                    sizeof(LutPush), "transmittance_gen") ||
      !make_compute(&multiscatter_gen_, REC_SHADER(k_multiscatter_lut_cs_hlsl), 1,
                    sizeof(LutPush), "multiscatter_gen")) {
    REC_ERROR("environment compute pipeline creation failed");
    return false;
  }

  // Set 2 of the mesh pipeline: ibl inputs, per frame ao, ddgi atlases, the
  // cascade shadow atlas (7) + cascade ubo (8), the opaque scene color (9,
  // sampled by transmissive materials for refraction), and the SIGMA-denoised
  // sun shadow (10, screen-space R8 sampled by the rt lighting variant).
  BindingLayoutDesc env_desc;
  env_desc.stages = kShaderStageFragment;
  for (u32 i = 0; i < 6; ++i) {
    env_desc.slots.push_back({i, BindingType::kCombinedTextureSampler});
  }
  env_desc.slots.push_back({6, BindingType::kUniformBuffer});
  env_desc.slots.push_back({7, BindingType::kCombinedTextureSampler});
  env_desc.slots.push_back({8, BindingType::kUniformBuffer});
  env_desc.slots.push_back({9, BindingType::kCombinedTextureSampler});
  env_desc.slots.push_back({10, BindingType::kCombinedTextureSampler});
  env_desc.slots.push_back({11, BindingType::kStorageBuffer});  // dynamic point lights
  // 12: NRD-denoised stochastic specular reflections (screen-space rgba16f),
  // sampled by the rt variant instead of tracing inline.
  env_desc.slots.push_back({12, BindingType::kCombinedTextureSampler});
  // 13/14: froxel cluster counts + light index list (light_cluster.cs).
  env_desc.slots.push_back({13, BindingType::kStorageBuffer});
  env_desc.slots.push_back({14, BindingType::kStorageBuffer});
  env_set_layout_ = device_.CreateBindingLayout(env_desc);
  if (!env_set_layout_) return false;

  return true;
}

bool EnvironmentSystem::CreateSkyPipeline(BindingLayoutHandle globals_layout, Format color_format,
                                          Format motion_format, Format depth_format) {
  // Sky background pipeline. Binding 0 is the sky cubemap; binding 1 is the
  // transmittance LUT, so the screen-space sun disc reddens/dims with the same
  // physical extinction as the sky rather than an air-mass approximation.
  // Depth: equal against the cleared reversed-z far value, no write: only
  // empty pixels shade. Blending is off on both targets; the old pipeline
  // masked attachment 1 (motion) writes to RG only, equivalent on the
  // two-channel motion format.
  sky_draw_pipeline_ = device_.CreateGraphicsPipeline({
      .vertex = REC_SHADER(k_fullscreen_vs_hlsl),
      .fragment = REC_SHADER(k_sky_ps_hlsl),
      .raster = {.cull = CullMode::kNone},
      .depth = {.test = true, .write = false, .compare = CompareOp::kEqual,
                .format = depth_format},
      .color_formats = {color_format, motion_format},
      .blend = {BlendMode::kOpaque, BlendMode::kOpaque},
      .sets = {{.shared = globals_layout},
               {.slots = {{0, BindingType::kCombinedTextureSampler},
                          {1, BindingType::kCombinedTextureSampler}},
                .stages = kShaderStageFragment}},
      .debug_name = "sky",
  });
  if (!sky_draw_pipeline_) {
    REC_ERROR("sky pipeline creation failed");
    return false;
  }
  return true;
}

bool EnvironmentSystem::BakeBrdfLut() {
  device_.ImmediateSubmit([&](CommandList& cmd) {
    cmd.Barrier(Transition(brdf_lut_, ResourceState::kUndefined, ResourceState::kGeneral));

    SizePush push{static_cast<f32>(kBrdfLutSize)};
    cmd.BindPipeline(brdf_gen_);
    cmd.BindTransient(0, {Bind::Storage(0, brdf_lut_)});
    cmd.Push(push);
    cmd.Dispatch(kBrdfLutSize / 8, kBrdfLutSize / 8, 1);

    cmd.Barrier(Transition(brdf_lut_, ResourceState::kGeneral,
                           ResourceState::kShaderReadFragment));
  });
  return true;
}

bool EnvironmentSystem::BakeLuts() {
  device_.ImmediateSubmit([&](CommandList& cmd) {
    // Transmittance LUT (no inputs).
    cmd.Barrier(Transition(transmittance_lut_, ResourceState::kUndefined,
                           ResourceState::kGeneral));
    LutPush tpush{{static_cast<f32>(kTransmittanceW), static_cast<f32>(kTransmittanceH)}};
    cmd.BindPipeline(transmittance_gen_);
    cmd.BindTransient(0, {Bind::Storage(0, transmittance_lut_)});
    cmd.Push(tpush);
    cmd.Dispatch2D({kTransmittanceW, kTransmittanceH});

    // Make it sampleable for the multiscatter pass (and the sky compute +
    // sky-draw fragment passes thereafter).
    cmd.Barrier(Transition(transmittance_lut_, ResourceState::kGeneral,
                           ResourceState::kShaderReadAll));

    // Multiple-scattering LUT (samples the transmittance LUT).
    cmd.Barrier(Transition(multiscatter_lut_, ResourceState::kUndefined,
                           ResourceState::kGeneral));
    LutPush mpush{{static_cast<f32>(kMultiScatterSize), static_cast<f32>(kMultiScatterSize)}};
    cmd.BindPipeline(multiscatter_gen_);
    cmd.BindTransient(0, {Bind::Storage(0, multiscatter_lut_),
                          Bind::Combined(1, transmittance_lut_.view, sampler_)});
    cmd.Push(mpush);
    cmd.Dispatch2D({kMultiScatterSize, kMultiScatterSize});

    cmd.Barrier(Transition(multiscatter_lut_, ResourceState::kGeneral,
                           ResourceState::kShaderReadCompute));
  });
  return true;
}

void EnvironmentSystem::RecordUpdate(CommandList& cmd, const Vec3& sun_direction,
                                     f32 sun_intensity, const Vec3& sun_color) {
  // The sky is sampled in both compute (convolutions, path tracers) and
  // fragment (sky draw); the convolutions only in fragment (mesh IBL).
  ResourceState sky_old =
      maps_initialized_ ? ResourceState::kShaderReadAll : ResourceState::kUndefined;
  ResourceState conv_old =
      maps_initialized_ ? ResourceState::kShaderReadFragment : ResourceState::kUndefined;
  maps_initialized_ = true;

  cmd.Barrier(Transition(sky_, sky_old, ResourceState::kGeneral));

  SkyPush sky_push{};
  Vec3 dir = Normalize(sun_direction);
  sky_push.sun_direction[0] = dir.x;
  sky_push.sun_direction[1] = dir.y;
  sky_push.sun_direction[2] = dir.z;
  sky_push.intensity = sun_intensity;
  sky_push.sun_color[0] = sun_color.x;
  sky_push.sun_color[1] = sun_color.y;
  sky_push.sun_color[2] = sun_color.z;
  sky_push.face_size = static_cast<f32>(kSkySize);
  cmd.BindPipeline(sky_gen_);
  cmd.BindTransient(0, {Bind::StorageView(0, sky_storage_view_),
                        Bind::Combined(1, transmittance_lut_.view, sampler_),
                        Bind::Combined(2, multiscatter_lut_.view, sampler_)});
  cmd.Push(sky_push);
  cmd.Dispatch(kSkySize / 8, kSkySize / 8, 6);

  cmd.Barrier(Transition(sky_, ResourceState::kGeneral, ResourceState::kShaderReadAll));

  cmd.Barrier(Transition(irradiance_, conv_old, ResourceState::kGeneral));
  cmd.Barrier(Transition(prefiltered_, conv_old, ResourceState::kGeneral));

  SizePush irradiance_push{static_cast<f32>(kIrradianceSize)};
  cmd.BindPipeline(irradiance_gen_);
  cmd.BindTransient(0, {Bind::StorageView(0, irradiance_storage_view_),
                        Bind::Combined(1, sky_.view, sampler_)});
  cmd.Push(irradiance_push);
  cmd.Dispatch(kIrradianceSize / 8 + 1, kIrradianceSize / 8 + 1, 6);

  cmd.BindPipeline(prefilter_gen_);
  for (u32 mip = 0; mip < kPrefilterMips; ++mip) {
    u32 size = kPrefilterSize >> mip;
    PrefilterPush push{static_cast<f32>(size),
                       static_cast<f32>(mip) / static_cast<f32>(kPrefilterMips - 1)};
    cmd.BindTransient(0, {Bind::StorageView(0, prefilter_storage_views_[mip]),
                          Bind::Combined(1, sky_.view, sampler_)});
    cmd.Push(push);
    cmd.Dispatch((size + 7) / 8, (size + 7) / 8, 6);
  }

  cmd.Barrier(Transition(irradiance_, ResourceState::kGeneral,
                         ResourceState::kShaderReadFragment));
  cmd.Barrier(Transition(prefiltered_, ResourceState::kGeneral,
                         ResourceState::kShaderReadFragment));
}

void EnvironmentSystem::DrawSky(CommandList& cmd, BindingSetHandle globals) {
  // Rebound under the sky pipeline: the mesh pipeline layout carries push
  // constant ranges this one lacks, which breaks set compatibility.
  cmd.BindPipeline(sky_draw_pipeline_);
  cmd.BindSet(0, globals);
  cmd.BindTransient(1, {Bind::Combined(0, sky_.view, sampler_),
                        Bind::Combined(1, transmittance_lut_.view, sampler_)});
  cmd.Draw(3);
}

void EnvironmentSystem::WriteEnvSet(BindingSetHandle set, TextureView ao_view,
                                    const DdgiBinding* ddgi, TextureView shadow_view,
                                    const GpuBuffer& cascade_buffer, u64 cascade_size,
                                    TextureView opaque_color, TextureView sun_shadow_view,
                                    const GpuBuffer& lights, u64 lights_size,
                                    TextureView spec_reflections,
                                    const GpuBuffer& cluster_counts,
                                    const GpuBuffer& cluster_indices) const {
  device_.UpdateBindingSet(
      set,
      {Bind::Combined(0, irradiance_.view, sampler_),
       Bind::Combined(1, prefiltered_.view, sampler_),
       Bind::Combined(2, brdf_lut_.view, sampler_),
       Bind::Combined(3, ao_view ? ao_view : white_.view, sampler_),
       // The live atlases stay in kGeneral (storage-written by the ddgi
       // compute passes each frame); the dummy is in the shader-read state.
       ddgi ? InGeneral(Bind::Combined(4, ddgi->irradiance, sampler_))
            : Bind::Combined(4, black_array_view_, sampler_),
       ddgi ? InGeneral(Bind::Combined(5, ddgi->distance, sampler_))
            : Bind::Combined(5, black_array_view_, sampler_),
       Bind::Uniform(6, ddgi ? ddgi->volume : dummy_volume_, 0,
                     ddgi ? ddgi->volume_size : 256),
       Bind::Combined(7, shadow_view ? shadow_view : shadow_dummy_.view, shadow_sampler_),
       Bind::Uniform(8, cascade_buffer ? cascade_buffer : dummy_volume_, 0,
                     cascade_buffer ? cascade_size : 512),
       Bind::Combined(9, opaque_color ? opaque_color : white_.view, sampler_),
       // white = fully lit
       Bind::Combined(10, sun_shadow_view ? sun_shadow_view : white_.view, sampler_),
       Bind::StorageBuffer(11, lights ? lights : dummy_storage_, 0, lights ? lights_size : 256),
       // The frame graph moves the denoised target to shader-read for the
       // scene pass (same as the sigma sun shadow); white when absent.
       Bind::Combined(12, spec_reflections ? spec_reflections : white_.view, sampler_),
       Bind::StorageBuffer(13, cluster_counts ? cluster_counts : dummy_storage_, 0,
                           cluster_counts ? cluster_counts.size : 256),
       Bind::StorageBuffer(14, cluster_indices ? cluster_indices : dummy_storage_, 0,
                           cluster_indices ? cluster_indices.size : 256)});
}

EnvironmentSystem::~EnvironmentSystem() {
  device_.DestroyPipeline(sky_draw_pipeline_);
  device_.DestroyBindingLayout(env_set_layout_);
  for (PipelineHandle* pipeline : {&sky_gen_, &irradiance_gen_, &prefilter_gen_, &brdf_gen_,
                                   &transmittance_gen_, &multiscatter_gen_}) {
    device_.DestroyPipeline(*pipeline);
    *pipeline = {};
  }
  device_.DestroyView(sky_storage_view_);
  device_.DestroyView(irradiance_storage_view_);
  for (TextureView view : prefilter_storage_views_) {
    device_.DestroyView(view);
  }
  device_.DestroyView(black_array_view_);
  device_.DestroyImage(sky_);
  device_.DestroyImage(irradiance_);
  device_.DestroyImage(prefiltered_);
  device_.DestroyImage(brdf_lut_);
  device_.DestroyImage(transmittance_lut_);
  device_.DestroyImage(multiscatter_lut_);
  device_.DestroyImage(white_);
  device_.DestroyImage(black_array_);
  device_.DestroyImage(shadow_dummy_);
  device_.DestroyBuffer(dummy_volume_);
  device_.DestroyBuffer(dummy_storage_);
  // Samplers are cached by the device and never destroyed by callers.
}

}  // namespace rec::render
