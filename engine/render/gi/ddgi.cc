#include "render/gi/ddgi.h"

#include <cmath>
#include <cstring>

#include "core/log.h"
#include "render/gi/raytracing.h"
#include "shaders/ddgi_blend_cs_hlsl.h"
#include "shaders/ddgi_border_cs_hlsl.h"
#include "shaders/ddgi_rays_cs_hlsl.h"

namespace rec::render {
namespace {

constexpr u32 kProbeCount = DdgiSystem::kProbesX * DdgiSystem::kProbesY * DdgiSystem::kProbesZ;
constexpr u32 kIrradianceWidth =
    (DdgiSystem::kIrradianceTexels + 2) * DdgiSystem::kProbesX * DdgiSystem::kProbesZ;
constexpr u32 kIrradianceHeight = (DdgiSystem::kIrradianceTexels + 2) * DdgiSystem::kProbesY;
constexpr u32 kDistanceWidth =
    (DdgiSystem::kDistanceTexels + 2) * DdgiSystem::kProbesX * DdgiSystem::kProbesZ;
constexpr u32 kDistanceHeight = (DdgiSystem::kDistanceTexels + 2) * DdgiSystem::kProbesY;

struct RaysPush {
  f32 rotation[12];  // three float4 rows
  f32 sun_direction[4];
  f32 sun_color[4];
};

struct BlendPush {
  f32 rotation[12];
  u32 mode;
  u32 ray_count;
  u32 reset;
  f32 pad;
};

struct BorderPush {
  u32 texels;
  u32 probes_x;
  u32 probes_y;
  u32 pad;
};

// Uniformly random rotation per frame so the fibonacci sphere covers all
// directions over time. Axis-angle from a weyl-sequence hash.
void FrameRotation(u32 frame_index, f32 out_rows[12]) {
  auto hash = [](u32 v) {
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    v ^= v >> 16;
    return v;
  };
  f32 u1 = static_cast<f32>(hash(frame_index) & 0xffffff) / 16777215.0f;
  f32 u2 = static_cast<f32>(hash(frame_index + 1) & 0xffffff) / 16777215.0f;
  f32 u3 = static_cast<f32>(hash(frame_index + 2) & 0xffffff) / 16777215.0f;
  f32 angle = u1 * 6.2831853f;
  f32 z = u2 * 2.0f - 1.0f;
  f32 r = std::sqrt(std::max(0.0f, 1.0f - z * z));
  f32 phi = u3 * 6.2831853f;
  Vec3 axis{r * std::cos(phi), r * std::sin(phi), z};

  f32 c = std::cos(angle);
  f32 s = std::sin(angle);
  f32 t = 1.0f - c;
  f32 rows[12] = {
      t * axis.x * axis.x + c,          t * axis.x * axis.y - s * axis.z,
      t * axis.x * axis.z + s * axis.y, 0,
      t * axis.x * axis.y + s * axis.z, t * axis.y * axis.y + c,
      t * axis.y * axis.z - s * axis.x, 0,
      t * axis.x * axis.z - s * axis.y, t * axis.y * axis.z + s * axis.x,
      t * axis.z * axis.z + c,          0,
  };
  std::memcpy(out_rows, rows, sizeof(rows));
}

}  // namespace

std::unique_ptr<DdgiSystem> DdgiSystem::Create(Device& device, TextureView sky_view,
                                               SamplerHandle sky_sampler,
                                               BindlessRegistry& bindless) {
  auto ddgi = std::unique_ptr<DdgiSystem>(new DdgiSystem(device));
  ddgi->sky_view_ = sky_view;
  ddgi->sky_sampler_ = sky_sampler;
  ddgi->bindless_ = &bindless;
  if (!ddgi->CreateResources(sky_view, sky_sampler) || !ddgi->CreatePipelines()) return nullptr;
  return ddgi;
}

void DdgiSystem::Configure(const Settings& settings) {
  if (settings.probe_spacing != settings_.probe_spacing) history_valid_ = false;
  settings_ = settings;
}

bool DdgiSystem::CreateResources(TextureView sky_view, SamplerHandle sky_sampler) {
  sampler_ = device_.GetSampler({.mip_filter = Filter::kNearest,
                                 .address_u = AddressMode::kClampToEdge,
                                 .address_v = AddressMode::kClampToEdge,
                                 .address_w = AddressMode::kClampToEdge,
                                 .max_lod = 0.0f});
  if (!sampler_) return false;

  TextureUsageFlags usage = kTextureUsageSampled | kTextureUsageStorage;
  irradiance_ =
      device_.CreateImage2D(Format::kRGBA16Float, {kIrradianceWidth, kIrradianceHeight}, usage);
  distance_ =
      device_.CreateImage2D(Format::kRGBA16Float, {kDistanceWidth, kDistanceHeight}, usage);
  rays_ = device_.CreateImage2D(Format::kRGBA16Float, {kRaysPerProbe, kProbeCount}, usage);
  if (!irradiance_ || !distance_ || !rays_) return false;

  // The shaders declare (RW)Texture2DArray over the atlases, so bind them
  // through explicit 2d-array views.
  irradiance_array_view_ = device_.CreateArrayView(irradiance_);
  distance_array_view_ = device_.CreateArrayView(distance_);
  if (!irradiance_array_view_ || !distance_array_view_) return false;

  for (GpuBuffer& buffer : volume_buffers_) {
    buffer = device_.CreateBuffer(sizeof(VolumeData), kBufferUsageUniform, true);
    if (!buffer.mapped) return false;
  }
  return true;
}

bool DdgiSystem::CreatePipelines() {
  // The rays pass resolves hit materials through the bindless set.
  rays_pipeline_ = device_.CreateComputePipeline({
      .shader = REC_SHADER(k_ddgi_rays_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kCombinedTextureSampler},
                          {2, BindingType::kCombinedTextureSampler},
                          {3, BindingType::kAccelStruct},
                          {4, BindingType::kUniformBuffer}}},
               {.shared = bindless_->set_layout()}},
      .push_constant_size = sizeof(RaysPush),
      .debug_name = "ddgi_rays",
  });
  blend_pipeline_ = device_.CreateComputePipeline({
      .shader = REC_SHADER(k_ddgi_blend_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kCombinedTextureSampler},
                          {2, BindingType::kUniformBuffer}}}},
      .push_constant_size = sizeof(BlendPush),
      .debug_name = "ddgi_blend",
  });
  border_pipeline_ = device_.CreateComputePipeline({
      .shader = REC_SHADER(k_ddgi_border_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage}}}},
      .push_constant_size = sizeof(BorderPush),
      .debug_name = "ddgi_border",
  });
  if (!rays_pipeline_ || !blend_pipeline_ || !border_pipeline_) {
    REC_ERROR("ddgi pipeline creation failed");
    return false;
  }
  return true;
}

EnvironmentSystem::DdgiBinding DdgiSystem::binding(u32 frame_index) const {
  EnvironmentSystem::DdgiBinding result;
  result.irradiance = irradiance_array_view_;
  result.distance = distance_array_view_;
  result.volume = volume_buffers_[frame_index % 2];
  result.volume_size = sizeof(VolumeData);
  result.layout = ResourceState::kGeneral;  // atlases live in GENERAL
  return result;
}

void DdgiSystem::AddToGraph(RenderGraph& graph, RayTracingContext& raytracing, u32 tlas_slot,
                            const Vec3& camera, const Vec3& sun_direction, f32 sun_intensity,
                            const Vec3& sun_color, u32 frame_index) {
  // Snap the volume to the probe grid around the camera; a snap shifts what
  // every probe represents, so history resets and re-converges.
  f32 spacing = settings_.probe_spacing;
  Vec3 extent{(kProbesX - 1) * spacing, (kProbesY - 1) * spacing, (kProbesZ - 1) * spacing};
  Vec3 origin{std::floor((camera.x - extent.x * 0.5f) / spacing) * spacing,
              std::floor((camera.y - extent.y * 0.5f) / spacing) * spacing,
              std::floor((camera.z - extent.z * 0.5f) / spacing) * spacing};
  bool snapped = origin.x != origin_.x || origin.y != origin_.y || origin.z != origin_.z;
  origin_ = origin;
  bool reset = !history_valid_ || snapped;
  history_valid_ = true;

  VolumeData volume{};
  volume.origin[0] = origin.x;
  volume.origin[1] = origin.y;
  volume.origin[2] = origin.z;
  volume.origin[3] = spacing;
  volume.counts[0] = kProbesX;
  volume.counts[1] = kProbesY;
  volume.counts[2] = kProbesZ;
  volume.counts[3] = kIrradianceTexels;
  volume.params[0] = static_cast<f32>(kDistanceTexels);
  volume.params[1] = settings_.hysteresis;
  volume.params[2] = spacing * 4.0f;  // max ray distance
  volume.params[3] = settings_.energy_scale;
  GpuBuffer& volume_buffer = volume_buffers_[frame_index % 2];
  std::memcpy(volume_buffer.mapped, &volume, sizeof(volume));

  RaysPush rays_push{};
  FrameRotation(frame_index, rays_push.rotation);
  Vec3 sun = Normalize(sun_direction);
  rays_push.sun_direction[0] = sun.x;
  rays_push.sun_direction[1] = sun.y;
  rays_push.sun_direction[2] = sun.z;
  rays_push.sun_direction[3] = sun_intensity;
  rays_push.sun_color[0] = sun_color.x;
  rays_push.sun_color[1] = sun_color.y;
  rays_push.sun_color[2] = sun_color.z;
  rays_push.sun_color[3] = static_cast<f32>(kRaysPerProbe);

  graph.AddPass(
      "ddgi", [](RenderGraph::PassBuilder&) {},
      [this, &raytracing, tlas_slot, rays_push, frame_index, reset](PassContext& ctx) {
        const GpuBuffer& volume_buffer = volume_buffers_[frame_index % 2];

        // Everything stays in GENERAL; first touch transitions from
        // UNDEFINED, after that plain memory barriers order the stages.
        if (!atlas_initialized_) {
          atlas_initialized_ = true;
          TextureBarrier barriers[3] = {
              Transition(irradiance_, ResourceState::kUndefined, ResourceState::kGeneral),
              Transition(distance_, ResourceState::kUndefined, ResourceState::kGeneral),
              Transition(rays_, ResourceState::kUndefined, ResourceState::kGeneral)};
          ctx.cmd->TextureBarriers(barriers);
        } else {
          // Last frame's fragment reads must finish before we rewrite.
          ctx.cmd->MemoryBarrier(BarrierScope::kGraphicsRead, BarrierScope::kComputeWrite);
        }

        // Probe rays.
        ctx.cmd->BindPipeline(rays_pipeline_);
        ctx.cmd->BindTransient(0, {Bind::Storage(0, rays_),
                                   Bind::Combined(1, sky_view_, sky_sampler_),
                                   InGeneral(Bind::Combined(2, irradiance_array_view_, sampler_)),
                                   Bind::Accel(3, raytracing.tlas(tlas_slot)),
                                   Bind::Uniform(4, volume_buffer, 0, sizeof(VolumeData))});
        ctx.cmd->BindSet(1, bindless_->set());
        ctx.cmd->Push(rays_push);
        ctx.cmd->Dispatch((kRaysPerProbe + 31) / 32, kProbeCount, 1);

        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);

        // Blend rays into both atlases.
        auto blend = [&](TextureView atlas_view, u32 mode, u32 width, u32 height) {
          BlendPush push{};
          std::memcpy(push.rotation, rays_push.rotation, sizeof(push.rotation));
          push.mode = mode;
          push.ray_count = kRaysPerProbe;
          push.reset = reset ? 1u : 0u;
          ctx.cmd->BindPipeline(blend_pipeline_);
          ctx.cmd->BindTransient(0, {Bind::StorageView(0, atlas_view),
                                     InGeneral(Bind::Combined(1, rays_.view, sampler_)),
                                     Bind::Uniform(2, volume_buffer, 0, sizeof(VolumeData))});
          ctx.cmd->Push(push);
          ctx.cmd->Dispatch2D({width, height});
        };
        blend(irradiance_array_view_, 0, kIrradianceWidth, kIrradianceHeight);
        blend(distance_array_view_, 1, kDistanceWidth, kDistanceHeight);

        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);

        // Octahedral borders for bilinear wrap.
        auto border = [&](TextureView atlas_view, u32 texels, u32 width, u32 height) {
          BorderPush push{texels, kProbesX * kProbesZ, kProbesY, 0};
          ctx.cmd->BindPipeline(border_pipeline_);
          ctx.cmd->BindTransient(0, {Bind::StorageView(0, atlas_view)});
          ctx.cmd->Push(push);
          ctx.cmd->Dispatch2D({width, height});
        };
        border(irradiance_array_view_, kIrradianceTexels, kIrradianceWidth, kIrradianceHeight);
        border(distance_array_view_, kDistanceTexels, kDistanceWidth, kDistanceHeight);

        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kGraphicsRead);
      });
}

DdgiSystem::~DdgiSystem() {
  for (PipelineHandle* p : {&rays_pipeline_, &blend_pipeline_, &border_pipeline_}) {
    device_.DestroyPipeline(*p);
    *p = {};
  }
  device_.DestroyView(irradiance_array_view_);
  device_.DestroyView(distance_array_view_);
  device_.DestroyImage(irradiance_);
  device_.DestroyImage(distance_);
  device_.DestroyImage(rays_);
  for (GpuBuffer& buffer : volume_buffers_) device_.DestroyBuffer(buffer);
  // sampler_ is device-cached, never destroyed by callers.
}

}  // namespace rec::render
