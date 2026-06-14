#ifndef RECREATION_RENDER_PARTICLES_H_
#define RECREATION_RENDER_PARTICLES_H_

#include <base/containers/vector.h>

#include "core/math.h"
#include "render/render_graph.h"
#include "render/rhi/device.h"

namespace rec::render {

class Device;

// One simulated particle, filled by the engine each frame and handed to the
// renderer through FrameView. Matches the Particle struct in particle.vs.
struct ParticleInstance {
  f32 pos[3] = {0, 0, 0};
  f32 size = 0.1f;
  f32 color[4] = {1, 1, 1, 1};  // rgb tint, a opacity
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
  // into), depth is the prepass reversed-z depth export (sampled for fade).
  void AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                  const base::Vector<ParticleInstance>& particles, const Frame& frame,
                  u32 frame_slot);

 private:
  static constexpr u32 kFramesInFlight = 2;
  static constexpr u32 kMaxParticles = 1u << 16;  // 65536

  Device* device_ = nullptr;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  GpuBuffer buffers_[kFramesInFlight];  // host-visible particle storage
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_PARTICLES_H_
