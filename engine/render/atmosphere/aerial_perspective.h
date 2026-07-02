#ifndef RECREATION_RENDER_AERIAL_PERSPECTIVE_H_
#define RECREATION_RENDER_AERIAL_PERSPECTIVE_H_

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/resources.h"

namespace rec::render {

class Device;

// Aerial perspective: composites the atmospheric scattering between the camera
// and each lit surface over the scene, so distant geometry hazes/blue-shifts
// like the sky. A short camera->surface raymarch sampling the Hillaire
// transmittance + multiple-scattering LUTs. Cheap (no ray tracing); runs every
// frame on the lit scene before the temporal pass.
class AerialPerspective {
 public:
  struct Frame {
    Mat4 inv_view_proj;
    Vec3 camera_pos;
    Vec3 sun_direction;  // travel direction
    f32 sun_intensity = 4.0f;
    Vec3 sun_color{1, 1, 1};
    f32 strength = 1.0f;  // 0 disables, 1 full physical effect
    u32 steps = 12;
  };

  bool Initialize(Device& device);
  void Destroy(Device& device);

  // Composites aerial perspective onto `color` (the lit scene) against `depth`,
  // sampling the atmosphere LUTs; returns the result. `transmittance` and
  // `multiscatter` are the EnvironmentSystem LUT views.
  ResourceHandle AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                            TextureView transmittance, TextureView multiscatter, Extent2D extent,
                            const Frame& frame);

 private:
  PipelineHandle pipeline_;
  SamplerHandle sampler_;  // linear clamp, for the LUTs
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_AERIAL_PERSPECTIVE_H_
