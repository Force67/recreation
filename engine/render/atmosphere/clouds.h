#ifndef RECREATION_RENDER_CLOUDS_H_
#define RECREATION_RENDER_CLOUDS_H_

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/resources.h"

namespace rec::render {

class Device;

// Volumetric clouds: a raymarched cloud layer composited over the lit scene
// (depth-aware, so terrain occludes clouds). Procedural density with Beer's-law
// self-shadowing and a Henyey-Greenstein phase. A simplified Nubis-style model;
// runs every frame, no ray tracing.
class Clouds {
 public:
  struct Frame {
    Mat4 inv_view_proj;
    Vec3 camera_pos;
    f32 time = 0.0f;  // seconds, drives wind animation
    Vec3 sun_direction;
    f32 sun_intensity = 4.0f;
    Vec3 sun_color{1, 1, 1};
    f32 coverage = 0.48f;   // 0 clear .. 1 overcast
    f32 bottom = 1500.0f;   // layer base, metres above sea level
    f32 top = 4200.0f;      // layer top
    f32 density = 1.0f;
    f32 wind = 12.0f;       // metres/second
    u32 steps = 32;
    u32 light_steps = 6;
  };

  bool Initialize(Device& device);
  void Destroy(Device& device);

  ResourceHandle AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                            Extent2D extent, const Frame& frame);

 private:
  PipelineHandle pipeline_;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_CLOUDS_H_
