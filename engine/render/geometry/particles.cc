#include "render/geometry/particles.h"

#include <algorithm>
#include <cstring>

#include "core/log.h"
#include "shaders/particle_ps_hlsl.h"
#include "shaders/particle_sim_cs_hlsl.h"
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
};

}  // namespace

bool ParticleSystem::Initialize(Device& device, Format color_format) {
  device_ = &device;

  // attachment 0 = lit colour, attachment 1 = motion. Both alpha-weighted so the
  // particle's velocity feeds the motion buffer where it is opaque.
  // TODO(rhi): blend preset mismatch: old alpha factors were ZERO/ONE (dst alpha
  // preserved); kAlpha uses ONE/ONE_MINUS_SRC_ALPHA.
  pipeline_ = device.CreateGraphicsPipeline({
      .vertex = REC_SHADER(k_particle_vs_hlsl),
      .fragment = REC_SHADER(k_particle_ps_hlsl),
      .topology = PrimitiveTopology::kTriangleStrip,
      .raster = {.cull = CullMode::kNone},
      .color_formats = {color_format, kParticleMotionFormat},
      .blend = {BlendMode::kAlpha, BlendMode::kAlpha},
      .sets = {{.slots = {{0, BindingType::kStorageBuffer},
                          {1, BindingType::kSampledImage}}}},
      .push_constant_size = sizeof(ParticlePush),
      .debug_name = "particles",
  });
  if (!pipeline_) {
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
                                const base::Vector<ParticleInstance>& particles, const Frame& frame,
                                u32 frame_slot) {
  if (particles.empty()) return;
  u32 count = std::min(static_cast<u32>(particles.size()), kMaxParticles);
  std::memcpy(buffers_[frame_slot].mapped, particles.data(), count * sizeof(ParticleInstance));
  GpuBuffer buffer = buffers_[frame_slot];

  graph.AddPass(
      "particles",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Write(color, ResourceUsage::kColorAttachment);
        builder.Write(motion, ResourceUsage::kColorAttachment);
        builder.Read(depth, ResourceUsage::kSampledFragment);
      },
      [this, color, depth, motion, buffer, count, frame](PassContext& ctx) {
        RecordDraw(ctx, color, depth, motion, buffer, count, frame);
      });
}

void ParticleSystem::RecordDraw(PassContext& ctx, ResourceHandle color, ResourceHandle depth,
                                ResourceHandle motion, const GpuBuffer& instances, u32 count,
                                const Frame& frame) {
  const GpuImage& target = ctx.graph->image(color);
  ColorAttachment attachments[2];
  attachments[0] = {.view = target.view, .load = LoadOp::kLoad};  // blend over the lit scene
  attachments[1] = {.view = ctx.graph->image(motion).view,
                    .load = LoadOp::kLoad};  // blend velocity over the mvecs
  ctx.cmd->BeginRendering({.extent = target.extent, .colors = attachments});

  ctx.cmd->BindPipeline(pipeline_);
  ctx.cmd->BindTransient(0, {Bind::StorageBuffer(0, instances, 0,
                                                 count * sizeof(ParticleInstance)),
                             Bind::Sampled(1, ctx.graph->image(depth))});

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
  ctx.cmd->Push(push);
  ctx.cmd->Draw(4, count, 0, 0);
  ctx.cmd->EndRendering();
}

void ParticleSystem::SimulateAndDraw(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                                     ResourceHandle motion, const Sim& sim, const Frame& frame,
                                     u32 frame_slot) {
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
      [this, color, depth, motion, instances, state, count, sim, frame](PassContext& ctx) {
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

        RecordDraw(ctx, color, depth, motion, instances, count, frame);
      });
}

void ParticleSystem::Destroy(Device& device) {
  device.DestroyPipeline(pipeline_);
  pipeline_ = {};
  device.DestroyPipeline(sim_pipeline_);
  sim_pipeline_ = {};
  device.DestroyBuffer(sim_state_);
  for (u32 i = 0; i < kFramesInFlight; ++i) device.DestroyBuffer(buffers_[i]);
}

}  // namespace rec::render
