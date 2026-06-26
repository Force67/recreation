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
  f32 pad = 0;
};

// Camera-facing billboard particle renderer. The engine owns the simulation;
// this uploads the live set to a per-frame storage buffer and draws lit, soft,
// depth-faded sprites into the resolved color before reconstruction. Alpha
// blended, no depth write; occlusion and soft fade come from the prepass depth.
class ParticleSystem {
 public:
  bool Initialize(Device& device, VkFormat color_format);
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
  };

  // Uploads particles into the frame slot's buffer and adds the draw pass.
  // No-op when particles is empty. color is the resolved scene color (blended
  // into), depth is the prepass reversed-z depth export, motion is the velocity
  // target the particles write into (for stable temporal reconstruction).
  void AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                  ResourceHandle motion, const base::Vector<ParticleInstance>& particles,
                  const Frame& frame, u32 frame_slot);

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
  };
  static constexpr u32 kMaxParticles = 1u << 18;  // 262144

  // Simulates the particles on the gpu (one compute dispatch over the persistent
  // state buffer) and draws the resulting billboards, in a single pass.
  void SimulateAndDraw(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                       ResourceHandle motion, const Sim& sim, const Frame& frame, u32 frame_slot);

 private:
  static constexpr u32 kFramesInFlight = 2;
  void RecordDraw(PassContext& ctx, ResourceHandle color, ResourceHandle depth,
                  ResourceHandle motion, VkBuffer instances, u32 count, const Frame& frame);

  Device* device_ = nullptr;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout sim_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout sim_layout_ = VK_NULL_HANDLE;
  VkPipeline sim_pipeline_ = VK_NULL_HANDLE;
  GpuBuffer buffers_[kFramesInFlight];  // host-visible billboard storage
  GpuBuffer sim_state_;                 // persistent gpu particle state
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_PARTICLES_H_
