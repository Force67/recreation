#ifndef RECREATION_RENDER_SURFACE_WEATHER_H_
#define RECREATION_RENDER_SURFACE_WEATHER_H_

#include "core/math.h"
#include "render/render_graph.h"
#include "render/rhi/resources.h"

namespace rec::render {

class Device;

// Surface weather: applies precipitation's effect on the world to the lit scene
// (rain wetness = darken + sky-reflection sheen on up-faces; snow = white
// accumulation on up-faces), using the G-buffer normals + depth and the sky
// cubemap for the puddle reflection. Cheap fullscreen pass; runs when wetness > 0.
class SurfaceWeather {
 public:
  struct Frame {
    Mat4 inv_view_proj;
    Vec3 camera_pos;
    f32 wetness = 0.0f;  // 0 dry .. 1 soaked
    bool snow = false;   // snow accumulation vs rain wetness
  };

  bool Initialize(Device& device);
  void Destroy(Device& device);

  // `normals`/`depth` are the G-buffer; `sky_view`/`sky_sampler` the sky cubemap.
  ResourceHandle AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle normals,
                            ResourceHandle depth, VkImageView sky_view, VkSampler sky_sampler,
                            VkExtent2D extent, const Frame& frame);

 private:
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_SURFACE_WEATHER_H_
