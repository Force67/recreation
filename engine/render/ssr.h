#ifndef RECREATION_RENDER_SSR_H_
#define RECREATION_RENDER_SSR_H_

#include "core/math.h"
#include "render/render_graph.h"
#include "render/rhi/resources.h"

namespace rec::render {

class Device;

// Screen-space reflections: a world-space ray march over the prepass depth and
// the lit scene color, no ray tracing required. This is the reflection fallback
// for tiers without ray query (mobile, low-end, forced low preset); rt tiers
// trace the tlas instead. It reads the opaque scene color and writes a copy
// with grazing-angle reflections blended in, so it slots between the scene pass
// and transparency without touching the mesh shader. TAA cleans the march noise.
class SsrPass {
 public:
  struct Settings {
    f32 intensity = 1.0f;       // reflection strength multiplier
    f32 max_distance = 12.0f;   // world-space march range, meters
    f32 thickness = 0.5f;       // depth-crossing tolerance, meters
    u32 step_count = 32;        // march samples per pixel
  };

  bool Initialize(Device& device);
  void Resize(Device& device, VkExtent2D extent) { extent_ = extent; }
  void Destroy(Device& device);

  void Configure(const Settings& settings) { settings_ = settings; }

  // Reflects opaque geometry in scene_color; returns a new color handle. view_proj
  // and inv_view_proj are the unjittered matrices; camera_pos is the world eye.
  ResourceHandle AddToGraph(RenderGraph& graph, ResourceHandle scene_color, ResourceHandle depth,
                            ResourceHandle normals, const Mat4& view_proj, const Mat4& inv_view_proj,
                            const Vec3& camera_pos, u32 frame_index);

 private:
  Settings settings_;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkExtent2D extent_{};
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_SSR_H_
