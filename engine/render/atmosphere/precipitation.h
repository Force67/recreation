#ifndef RECREATION_RENDER_PRECIPITATION_H_
#define RECREATION_RENDER_PRECIPITATION_H_

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/resources.h"

namespace rec::render {

class Device;

// Screen-space precipitation (rain streaks / snow flakes) composited over the
// lit scene, driven by the weather system. Procedural, world-anchored, cheap; no
// particle simulation. Runs every frame when intensity > 0.
class Precipitation {
 public:
  struct Frame {
    Mat4 inv_view_proj;
    Vec3 camera_pos;
    f32 time = 0.0f;     // seconds, drives the fall animation
    f32 intensity = 0.0f;  // 0 none .. 1 heavy
    bool snow = false;     // snow flakes vs rain streaks
  };

  bool Initialize(Device& device);
  void Destroy(Device& device);

  ResourceHandle AddToGraph(RenderGraph& graph, ResourceHandle color, VkExtent2D extent,
                            const Frame& frame);

 private:
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_PRECIPITATION_H_
