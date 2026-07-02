#ifndef RECREATION_RENDER_REFLECTION_TRACE_H_
#define RECREATION_RENDER_REFLECTION_TRACE_H_

// Stochastic specular reflections: one VNDF-sampled GGX ray per pixel through
// the TLAS, hit-shaded like the forward rt variant's inline mirror ray (plus
// a sun shadow ray), packed for NRD REBLUR_SPECULAR. The denoised result is
// sampled by the forward pass instead of tracing inline, which turns the
// mirror-to-IBL crossfade into a real glossy distribution.

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rec::render {

class RayTracingContext;

class ReflectionTrace {
 public:
  struct Frame {
    Mat4 inv_view_proj;  // unjittered
    Vec3 camera_pos;
    Vec3 sun_direction;  // travel direction
    f32 sun_intensity = 1.0f;
    Vec3 sun_color{1.0f, 1.0f, 1.0f};
    f32 roughness_cutoff = 0.6f;
    u32 frame_index = 0;
    f32 near_plane = 0.1f;
    const f32* hit_dist_params = nullptr;  // NrdDenoiser::kHitDistParams
    bool ddgi = false;
  };

  bool Initialize(Device& device, BindingLayoutHandle bindless_layout);
  void Destroy(Device& device);
  bool available() const { return static_cast<bool>(pipeline_); }

  // Returns the packed radiance+hitdist target (rgba16f) for DenoiseSpecular.
  ResourceHandle AddToGraph(RenderGraph& graph, RayTracingContext& raytracing, u32 tlas_slot,
                            BindingSetHandle bindless_set, ResourceHandle depth,
                            ResourceHandle normals, TextureView prefiltered,
                            TextureView ddgi_irradiance, bool ddgi_in_general,
                            const GpuBuffer& ddgi_volume, u64 ddgi_volume_size,
                            SamplerHandle sampler, Extent2D extent, const Frame& frame);

 private:
  PipelineHandle pipeline_;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_REFLECTION_TRACE_H_
