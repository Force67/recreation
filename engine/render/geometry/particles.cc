#include "render/geometry/particles.h"

#include <algorithm>
#include <cstring>

#include "core/log.h"
#include "shaders/particle_ps_hlsl.h"
#include "shaders/particle_sim_cs_hlsl.h"
#include "shaders/particle_tex_ps_hlsl.h"
#include "shaders/particle_vs_hlsl.h"

namespace rec::render {
namespace {

constexpr Format kParticleMotionFormat = Format::kRG16Float;  // == kMotionFormat

struct ParticlePush {
  Mat4 view_proj;
  f32 cam_right[3];
  f32 near_plane;
  f32 cam_up[3];
  f32 soft_fade;
  f32 sun_dir[3];
  f32 sun_intensity;
  f32 sun_color[3];
  f32 ambient;
  Mat4 prev_view_proj;
  u32 emissive;
  f32 pad[3];
  f32 cluster_params[4];
  f32 froxel_params[4];
};

struct ParticleSimPush {
  f32 emitter[3];
  f32 dt;
  f32 gravity;
  f32 spawn_speed;
  f32 life_min;
  f32 life_range;
  f32 size_min;
  f32 size_range;
  u32 count;
  u32 frame;
  u32 mode;
  f32 radius;
  f32 intensity;
  f32 time;
  f32 pad[2];
};

}  // namespace

bool ParticleSystem::Initialize(Device& device, Format color_format,
                                BindingLayoutHandle bindless_layout) {
  device_ = &device;
  bindless_layout_ = bindless_layout;
  bool textured = static_cast<bool>(bindless_layout);

  // Set 0 is the per-frame draw inputs; when a bindless table is available it
  // binds as set 1 and the billboards sample their authored effect texture.
  base::Vector<PipelineBindings> sets;
  sets.push_back({.slots = {{0, BindingType::kStorageBuffer},
                            {1, BindingType::kSampledImage},
                            {2, BindingType::kStorageBuffer},
                            {3, BindingType::kStorageBuffer},
                            {4, BindingType::kStorageBuffer},
                            {5, BindingType::kStorageBuffer},
                            {6, BindingType::kCombinedTextureSampler},
                            {7, BindingType::kCombinedTextureSampler}}});
  if (textured) sets.push_back({.shared = bindless_layout_});
  ShaderBlob frag =
      textured ? REC_SHADER(k_particle_tex_ps_hlsl) : REC_SHADER(k_particle_ps_hlsl);

  // attachment 0 = lit colour, attachment 1 = motion. Both alpha-weighted so the
  // particle's velocity feeds the motion buffer where it is opaque.
  // TODO(rhi): blend preset mismatch: old alpha factors were ZERO/ONE (dst alpha
  // preserved); kAlpha uses ONE/ONE_MINUS_SRC_ALPHA.
  GraphicsPipelineDesc desc{
      .vertex = REC_SHADER(k_particle_vs_hlsl),
      .fragment = frag,
      .topology = PrimitiveTopology::kTriangleStrip,
      .raster = {.cull = CullMode::kNone},
      .color_formats = {color_format, kParticleMotionFormat},
      .blend = {BlendMode::kAlpha, BlendMode::kAlpha},
      .sets = sets,
      .push_constant_size = sizeof(ParticlePush),
      .debug_name = "particles",
  };
  pipeline_ = device.CreateGraphicsPipeline(desc);
  // Fire path: HDR additive color, motion still alpha-weighted.
  desc.blend = {BlendMode::kAdditive, BlendMode::kAlpha};
  desc.debug_name = "particles_additive";
  pipeline_additive_ = device.CreateGraphicsPipeline(desc);
  if (!pipeline_ || !pipeline_additive_) {
    REC_ERROR("particle pipeline creation failed");
    return false;
  }

  for (u32 i = 0; i < kFramesInFlight; ++i) {
    buffers_[i] = device.CreateBuffer(static_cast<u64>(kMaxParticles) * sizeof(ParticleInstance),
                                      kBufferUsageStorage, true);
    if (!buffers_[i].mapped) return false;
  }

  // GPU simulation: a compute pipeline over the persistent state buffer.
  sim_pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_particle_sim_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageBuffer},
                          {1, BindingType::kStorageBuffer}}}},
      .push_constant_size = sizeof(ParticleSimPush),
      .debug_name = "particle_sim",
  });
  if (!sim_pipeline_) {
    REC_ERROR("particle sim pipeline creation failed");
    return false;
  }

  // 64 bytes per state entry; zero-init so every particle's seed is 0 and spawns
  // on first touch.
  sim_state_ =
      device.CreateBuffer(static_cast<u64>(kMaxParticles) * 64, kBufferUsageStorage, true);
  if (!sim_state_.mapped) return false;
  std::memset(sim_state_.mapped, 0, static_cast<size_t>(kMaxParticles) * 64);
  return true;
}

void ParticleSystem::AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                                ResourceHandle motion,
                                const base::Vector<ParticleInstance>& particles,
                                const base::Vector<ParticleInstance>& additive, const Frame& frame,
                                u32 frame_slot, BindingSetHandle bindless) {
  // Both sets share the slot's buffer: lit at 0, additive at the next
  // 256-aligned offset (safe for any minStorageBufferOffsetAlignment).
  u32 count = std::min(static_cast<u32>(particles.size()), kMaxParticles);
  u64 additive_offset = (static_cast<u64>(count) * sizeof(ParticleInstance) + 255) & ~255ull;
  u64 capacity = static_cast<u64>(kMaxParticles) * sizeof(ParticleInstance);
  u64 additive_room = (capacity - std::min(capacity, additive_offset)) / sizeof(ParticleInstance);
  u32 additive_count =
      std::min(static_cast<u32>(additive.size()), static_cast<u32>(additive_room));
  if (count == 0 && additive_count == 0) return;
  u8* mapped = static_cast<u8*>(buffers_[frame_slot].mapped);
  if (count > 0) std::memcpy(mapped, particles.data(), count * sizeof(ParticleInstance));
  if (additive_count > 0) {
    std::memcpy(mapped + additive_offset, additive.data(),
                additive_count * sizeof(ParticleInstance));
  }
  GpuBuffer buffer = buffers_[frame_slot];

  graph.AddPass(
      "particles",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Write(color, ResourceUsage::kColorAttachment);
        builder.Write(motion, ResourceUsage::kColorAttachment);
        builder.Read(depth, ResourceUsage::kSampledFragment);
      },
      [this, color, depth, motion, buffer, count, additive_offset, additive_count, frame,
       bindless](PassContext& ctx) {
        const GpuImage& target = ctx.graph->image(color);
        ColorAttachment attachments[2];
        attachments[0] = {.view = target.view, .load = LoadOp::kLoad};
        attachments[1] = {.view = ctx.graph->image(motion).view, .load = LoadOp::kLoad};
        ctx.cmd->BeginRendering({.extent = target.extent, .colors = attachments});
        if (count > 0) RecordSet(ctx, depth, buffer, 0, count, frame, frame.emissive, bindless);
        if (additive_count > 0) {
          RecordSet(ctx, depth, buffer, additive_offset, additive_count, frame, true, bindless);
        }
        ctx.cmd->EndRendering();
      });
}

void ParticleSystem::RecordDraw(PassContext& ctx, ResourceHandle color, ResourceHandle depth,
                                ResourceHandle motion, const GpuBuffer& instances, u32 count,
                                const Frame& frame, BindingSetHandle bindless) {
  const GpuImage& target = ctx.graph->image(color);
  ColorAttachment attachments[2];
  attachments[0] = {.view = target.view, .load = LoadOp::kLoad};  // blend over the lit scene
  attachments[1] = {.view = ctx.graph->image(motion).view,
                    .load = LoadOp::kLoad};  // blend velocity over the mvecs
  ctx.cmd->BeginRendering({.extent = target.extent, .colors = attachments});
  RecordSet(ctx, depth, instances, 0, count, frame, frame.emissive, bindless);
  ctx.cmd->EndRendering();
}

void ParticleSystem::RecordSet(PassContext& ctx, ResourceHandle depth, const GpuBuffer& instances,
                               u64 offset, u32 count, const Frame& frame, bool emissive,
                               BindingSetHandle bindless) {
  ctx.cmd->BindPipeline(emissive ? pipeline_additive_ : pipeline_);
  if (bindless_layout_ && bindless) ctx.cmd->BindSet(1, bindless);
  // The froxel volume stays in GENERAL; every other input arrives shader-read.
  ctx.cmd->BindTransient(
      0, {Bind::StorageBuffer(0, instances, offset, count * sizeof(ParticleInstance)),
          Bind::Sampled(1, ctx.graph->image(depth)),
          Bind::StorageBuffer(2, frame.lights, 0, frame.lights.size),
          Bind::StorageBuffer(3, frame.cluster_counts, 0, frame.cluster_counts.size),
          Bind::StorageBuffer(4, frame.cluster_indices, 0, frame.cluster_indices.size),
          Bind::StorageBuffer(5, frame.local_shadow_faces, 0, frame.local_shadow_faces.size),
          Bind::Combined(6, frame.local_shadow_atlas, frame.comparison_sampler),
          InGeneral(Bind::Combined(7, frame.froxel_volume, frame.froxel_sampler))});

  ParticlePush push{};
  push.view_proj = frame.view_proj;
  push.cam_right[0] = frame.cam_right.x;
  push.cam_right[1] = frame.cam_right.y;
  push.cam_right[2] = frame.cam_right.z;
  push.near_plane = frame.near_plane;
  push.cam_up[0] = frame.cam_up.x;
  push.cam_up[1] = frame.cam_up.y;
  push.cam_up[2] = frame.cam_up.z;
  push.soft_fade = frame.soft_fade;
  push.sun_dir[0] = frame.sun_direction.x;
  push.sun_dir[1] = frame.sun_direction.y;
  push.sun_dir[2] = frame.sun_direction.z;
  push.sun_intensity = frame.sun_intensity;
  push.sun_color[0] = frame.sun_color.x;
  push.sun_color[1] = frame.sun_color.y;
  push.sun_color[2] = frame.sun_color.z;
  push.ambient = frame.ambient;
  push.prev_view_proj = frame.prev_view_proj;
  std::memcpy(push.cluster_params, frame.cluster_params, sizeof(push.cluster_params));
  push.froxel_params[0] = frame.froxel_near;
  push.froxel_params[1] = frame.froxel_far;
  push.froxel_params[2] = frame.froxel_enabled ? 1.0f : 0.0f;
  push.froxel_params[3] = 0.0f;
  push.emissive = emissive ? 1u : 0u;
  ctx.cmd->Push(push);
  ctx.cmd->Draw(4, count, 0, 0);
}

void ParticleSystem::SimulateAndDraw(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                                     ResourceHandle motion, const Sim& sim, const Frame& frame,
                                     u32 frame_slot, BindingSetHandle bindless) {
  u32 count = std::min(sim.count, kMaxParticles);
  if (count == 0) return;
  GpuBuffer instances = buffers_[frame_slot];
  GpuBuffer state = sim_state_;

  graph.AddPass(
      "gpu_particles",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Write(color, ResourceUsage::kColorAttachment);
        builder.Write(motion, ResourceUsage::kColorAttachment);
        builder.Read(depth, ResourceUsage::kSampledFragment);
      },
      [this, color, depth, motion, instances, state, count, sim, frame,
       bindless](PassContext& ctx) {
        // Step the simulation, then draw the freshly written billboards.
        ParticleSimPush sp{};
        sp.emitter[0] = sim.emitter[0];
        sp.emitter[1] = sim.emitter[1];
        sp.emitter[2] = sim.emitter[2];
        sp.dt = sim.dt < 0.05f ? sim.dt : 0.05f;  // clamp hitches
        sp.gravity = sim.gravity;
        sp.spawn_speed = sim.spawn_speed;
        sp.life_min = sim.life_min;
        sp.life_range = sim.life_range;
        sp.size_min = sim.size_min;
        sp.size_range = sim.size_range;
        sp.count = count;
        sp.frame = 0x9e3779b9u ^ count;  // nonzero seed salt; per-particle index varies it
        sp.mode = sim.mode;
        sp.radius = sim.radius;
        sp.intensity = sim.intensity;
        sp.time = sim.time;
        ctx.cmd->BindPipeline(sim_pipeline_);
        ctx.cmd->BindTransient(0, {Bind::StorageBuffer(0, state),
                                   Bind::StorageBuffer(1, instances, 0,
                                                       count * sizeof(ParticleInstance))});
        ctx.cmd->Push(sp);
        ctx.cmd->Dispatch((count + 63) / 64, 1, 1);

        // The instance writes must be visible to the vertex pull; the state
        // writes to the next frame's sim (same queue, ordered).
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kGraphicsRead);
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);

        RecordDraw(ctx, color, depth, motion, instances, count, frame, bindless);
      });
}

void ParticleSystem::Destroy(Device& device) {
  device.DestroyPipeline(pipeline_);
  pipeline_ = {};
  device.DestroyPipeline(pipeline_additive_);
  pipeline_additive_ = {};
  device.DestroyPipeline(sim_pipeline_);
  sim_pipeline_ = {};
  device.DestroyBuffer(sim_state_);
  for (u32 i = 0; i < kFramesInFlight; ++i) device.DestroyBuffer(buffers_[i]);
}

}  // namespace rec::render
