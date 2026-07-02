#ifndef RECREATION_RENDER_DEPTH_OF_FIELD_H_
#define RECREATION_RENDER_DEPTH_OF_FIELD_H_

// Bokeh depth of field: signed CoC with eased in-shader autofocus (no cpu
// readback), a half-res 48-tap golden-spiral gather, and a full-res
// composite. Runs on the AA-resolved color, before motion blur.

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rec::render {

class DepthOfFieldPass {
 public:
  bool Initialize(Device& device);
  void Destroy(Device& device);

  struct Frame {
    f32 aperture = 6.0f;        // coc pixels per unit relative defocus
    f32 max_coc = 24.0f;        // pixels
    f32 focus_distance = 0.0f;  // meters; <= 0 enables center autofocus
    f32 focus_speed = 0.08f;
    f32 near_plane = 0.1f;
  };

  // color is the AA-resolved scene at output resolution; depth is the
  // render-res reversed-z export (uv-sampled, so resolutions may differ).
  ResourceHandle AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                            Extent2D extent, const Frame& frame);

 private:
  PipelineHandle coc_pipeline_;
  PipelineHandle gather_pipeline_;
  PipelineHandle composite_pipeline_;
  GpuBuffer focus_state_;
  SamplerHandle sampler_;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_DEPTH_OF_FIELD_H_
