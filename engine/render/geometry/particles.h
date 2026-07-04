#ifndef RECREATION_RENDER_PARTICLES_H_
#define RECREATION_RENDER_PARTICLES_H_

#include <base/containers/vector.h>

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rec::render {

class Device;

// One simulated particle, filled by the engine each frame and handed to the
// renderer through FrameView. Matches the Particle struct in particle.vs.
struct ParticleInstance {
  f32 pos[3] = {0, 0, 0};
  f32 size = 0.1f;
  f32 color[4] = {1, 1, 1, 1};  // rgb tint, a opacity
  f32 prev_pos[3] = {0, 0, 0};  // last frame's centre, for the motion vector
  // Bindless index of the authored effect texture sampled on the billboard;
  // 0xffffffff keeps the procedural (untextured) gaussian sprite.
  u32 tex = 0xffffffffu;
};

// Camera-facing billboard particle renderer. The engine owns the simulation;
// this uploads the live set to a per-frame storage buffer and draws lit, soft,
// depth-faded sprites into the resolved color before reconstruction. Alpha
// blended, no depth write; occlusion and soft fade come from the prepass depth.
class ParticleSystem {
 public:
  // bindless_layout binds the engine's bindless texture table as set 1 so the
  // billboards can sample their authored effect texture; pass a null handle to
  // keep the procedural sprites (no bindless device).
  bool Initialize(Device& device, Format color_format, BindingLayoutHandle bindless_layout = {});
  void Destroy(Device& device);

  struct Frame {
    Mat4 view_proj;
    Mat4 prev_view_proj;  // for the particle motion vectors
    Vec3 cam_right;
    Vec3 cam_up;
    Vec3 sun_direction;  // travel direction
    Vec3 sun_color;
    f32 sun_intensity = 4.0f;
    f32 ambient = 0.1f;
    f32 near_plane = 0.1f;
    f32 soft_fade = 0.5f;  // meters of view-z fade as a particle nears geometry
    // HDR additive mode (fire): particle color is radiance, drawn with an
    // additive blend instead of lit alpha.
    bool emissive = false;
    // Lit translucency: clustered lights (with local shadow maps) wrap the
    // puffs and the froxel volume's transmittance dims them with the fog.
    f32 cluster_params[4] = {0, 0, 64, 64};
    f32 froxel_near = 0.1f;
    f32 froxel_far = 64.0f;
    bool froxel_enabled = false;
    GpuBuffer lights;
    GpuBuffer cluster_counts;
    GpuBuffer cluster_indices;
    GpuBuffer local_shadow_faces;
    TextureView local_shadow_atlas;
    SamplerHandle comparison_sampler;
    TextureView froxel_volume;
    SamplerHandle froxel_sampler;
  };

  // Uploads both billboard sets into the frame slot's buffer and adds one draw
  // pass: `particles` draws with the lit alpha pipeline (frame.emissive picks
  // additive instead, as before), `additive` always with the HDR additive one
  // (fire from the NIF emitters, beside smoke in the same frame). No-op when
  // both are empty. color is the resolved scene color (blended into), depth is
  // the prepass reversed-z depth export, motion is the velocity target the
  // particles write into (for stable temporal reconstruction).
  void AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                  ResourceHandle motion, const base::Vector<ParticleInstance>& particles,
                  const base::Vector<ParticleInstance>& additive, const Frame& frame,
                  u32 frame_slot, BindingSetHandle bindless = {});

  // Fountain emitter parameters for the gpu simulation.
  struct Sim {
    f32 emitter[3] = {0, 0, 0};
    f32 dt = 0.0f;
    u32 count = 0;        // live particle count (fixed; dead ones respawn)
    f32 gravity = 4.0f;
    f32 spawn_speed = 4.5f;
    f32 life_min = 1.6f;
    f32 life_range = 0.8f;
    f32 size_min = 0.12f;
    f32 size_range = 0.10f;
    u32 mode = 0;          // 0 ember fountain, 1 fire (buoyant flames + embers)
    f32 radius = 0.3f;     // fire emitter disk radius
    f32 intensity = 1.0f;  // fire emissive scale
    f32 time = 0.0f;       // seconds, drives the turbulence field
  };
  static constexpr u32 kMaxParticles = 1u << 18;  // 262144

  // Simulates the particles on the gpu (one compute dispatch over the persistent
  // state buffer) and draws the resulting billboards, in a single pass.
  void SimulateAndDraw(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                       ResourceHandle motion, const Sim& sim, const Frame& frame, u32 frame_slot,
                       BindingSetHandle bindless = {});

 private:
  static constexpr u32 kFramesInFlight = 2;
  void RecordDraw(PassContext& ctx, ResourceHandle color, ResourceHandle depth,
                  ResourceHandle motion, const GpuBuffer& instances, u32 count, const Frame& frame,
                  BindingSetHandle bindless);
  // One pipeline + bind + draw inside an open rendering pass; offset is the
  // byte offset of this set's instances in the shared buffer.
  void RecordSet(PassContext& ctx, ResourceHandle depth, const GpuBuffer& instances, u64 offset,
                 u32 count, const Frame& frame, bool emissive, BindingSetHandle bindless);

  Device* device_ = nullptr;
  BindingLayoutHandle bindless_layout_;  // engine bindless texture table (set 1)
  PipelineHandle pipeline_;
  PipelineHandle pipeline_additive_;
  PipelineHandle sim_pipeline_;
  GpuBuffer buffers_[kFramesInFlight];  // host-visible billboard storage
  GpuBuffer sim_state_;                 // persistent gpu particle state
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_PARTICLES_H_
