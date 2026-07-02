#ifndef RECREATION_RENDER_VOLUMETRIC_FOG_H_
#define RECREATION_RENDER_VOLUMETRIC_FOG_H_

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/resources.h"

namespace rec::render {

class Device;
class RayTracingContext;

// Ray-marched volumetric fog with shadowed sun single-scattering (god rays).
// Composites over the lit scene before the temporal pass resolves the marched
// noise. Needs ray query for the per-step shadow test.
class VolumetricFog {
 public:
  struct Frame {
    Mat4 inv_view_proj;
    Vec3 camera_pos;
    Vec3 sun_direction;  // travel direction
    f32 sun_intensity = 4.0f;
    Vec3 sun_color{1, 1, 1};
    f32 density = 0.02f;
    f32 height_falloff = 0.15f;
    f32 base_height = 0.0f;
    f32 anisotropy = 0.6f;     // henyey-greenstein g, forward scattering
    f32 max_distance = 200.0f;
    u32 steps = 32;
    u32 frame_index = 0;
  };

  bool Initialize(Device& device);
  void Destroy(Device& device);

  // Marches color (the lit scene) against depth, returns a fogged copy.
  ResourceHandle AddToGraph(RenderGraph& graph, RayTracingContext& raytracing, u32 tlas_slot,
                            ResourceHandle color, ResourceHandle depth, Extent2D extent,
                            const Frame& frame);

 private:
  PipelineHandle pipeline_;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_VOLUMETRIC_FOG_H_
