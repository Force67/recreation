#ifndef RECREATION_RENDER_SSGI_H_
#define RECREATION_RENDER_SSGI_H_

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/resources.h"

namespace rec::render {

class Device;

// Screen-space global illumination: a single diffuse bounce gathered from the
// prepass depth + the lit scene color, no ray tracing required. This is the gi
// fallback for tiers without ray query (mobile, low-end, forced low preset); rt
// tiers use the ddgi probe volume instead. It reads the opaque scene color and
// writes a copy with the bounce added, so it slots between the scene pass and
// transparency without touching the mesh shader. TAA cleans the gather noise.
class SsgiPass {
 public:
  struct Settings {
    f32 radius = 2.2f;      // gather radius, meters
    f32 intensity = 4.0f;   // bounce strength
    u32 sample_count = 24;  // hemisphere taps per pixel
  };

  bool Initialize(Device& device);
  void Resize(Device& device, VkExtent2D extent) { extent_ = extent; }
  void Destroy(Device& device);

  void Configure(const Settings& settings) { settings_ = settings; }

  // proj_scale is {proj.m[0], proj.m[5]}. Returns a new color handle with the
  // bounce blended into scene_color.
  ResourceHandle AddToGraph(RenderGraph& graph, ResourceHandle scene_color, ResourceHandle depth,
                            ResourceHandle normals, const Mat4& inv_view_proj,
                            const f32 proj_scale[2], f32 near_plane, u32 frame_index);

 private:
  Settings settings_;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkExtent2D extent_{};
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_SSGI_H_
