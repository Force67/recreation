#ifndef RECREATION_RENDER_SHADOW_TRACE_H_
#define RECREATION_RENDER_SHADOW_TRACE_H_

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/resources.h"

namespace rec::render {

class Device;
class RayTracingContext;

// Screen-space sun-shadow ray trace: one ray per pixel toward the sun through
// the frame TLAS, writing NRD's SIGMA IN_PENUMBRA (the packed occluder distance
// sized by the sun's angular radius). SIGMA owns the spatial/temporal filtering;
// the denoised result is sampled by the lighting pass. Needs ray query and a
// live NRD instance; the renderer only runs it then.
class ShadowTracePass {
 public:
  bool Initialize(Device& device);
  void Resize(Device& device, Extent2D extent) { extent_ = extent; }
  void Destroy(Device& device);

  // Adds the trace pass and returns the packed penumbra (R16f). sun_direction is
  // the light's travel direction; angular_radius is the sun's half-angle (rad).
  ResourceHandle AddToGraph(RenderGraph& graph, RayTracingContext& raytracing, u32 tlas_slot,
                            ResourceHandle depth, const Mat4& inv_view_proj,
                            const Vec3& sun_direction, f32 near_plane, f32 angular_radius,
                            f32 jitter_x, f32 jitter_y);

 private:
  PipelineHandle pipeline_;
  Extent2D extent_{};
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_SHADOW_TRACE_H_
