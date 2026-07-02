#ifndef RECREATION_RENDER_RESTIR_DI_H_
#define RECREATION_RENDER_RESTIR_DI_H_

// Hybrid-path ReSTIR DI: reservoir-based many-light direct lighting for the
// raster forward pass. Per-pixel streaming RIS over the clustered point/spot
// lights on the prepass G-buffer, temporal + spatial reuse, then one inline
// shadow ray for each pixel's winning light. Outputs demodulated diffuse and
// (F-less) specular radiance textures the scene pass folds back in, replacing
// both the analytic cluster evaluation and the local shadow atlas for those
// lights with per-pixel ray-traced visibility.

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/gi/raytracing.h"
#include "render/rhi/device.h"

namespace rec::render {

class RestirDi {
 public:
  bool Initialize(Device& device);
  void Destroy(Device& device);
  bool Resize(Device& device, Extent2D extent);
  bool available() const {
    return static_cast<bool>(temporal_pipeline_) && static_cast<bool>(reservoir_[0]);
  }

  struct Frame {
    Mat4 inv_view_proj;  // unjittered
    Vec3 camera_pos;
    u32 frame_index = 0;
    u32 light_count = 0;
    GpuBuffer lights;
    u32 tlas_slot = 0;
  };

  struct Outputs {
    ResourceHandle diffuse = kInvalidResource;  // irradiance (multiply by albedo/pi)
    ResourceHandle spec = kInvalidResource;     // GGX D*V*ndl (multiply by f0)
  };

  // depth/normals/motion are the prepass exports. Records temporal + spatial
  // stages; the returned graph textures are ready for fragment sampling by
  // the scene pass.
  Outputs AddToGraph(RenderGraph& graph, ResourceHandle depth_export, ResourceHandle normals,
                     ResourceHandle motion, RayTracingContext& raytracing, Extent2D extent,
                     const Frame& frame);

 private:
  PipelineHandle temporal_pipeline_;
  PipelineHandle spatial_pipeline_;
  // reservoir_[0]: temporal output (scratch), reservoir_[1]: spatial output =
  // next frame's history. Fixed roles, no swap.
  GpuImage reservoir_[2];
  GpuImage prev_depth_;   // R32F snapshot for reprojection validation
  GpuImage prev_normal_;  // RGBA16F oct+roughness snapshot
  Extent2D extent_ = {0, 0};
  bool reset_ = true;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_RESTIR_DI_H_
