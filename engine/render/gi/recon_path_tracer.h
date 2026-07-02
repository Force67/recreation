#ifndef RECREATION_RENDER_RECON_PATH_TRACER_H_
#define RECREATION_RENDER_RECON_PATH_TRACER_H_

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/resources.h"

namespace rec::render {

class Device;
class RayTracingContext;

// SVGF-style reconstruction path tracer (the "gameplay" mode), separate from the
// brute-force reference and the NRD-denoised path. Traces one noisy sample per
// pixel into a g-buffer + demodulated diffuse irradiance, then reconstructs with
// its own temporal accumulation (motion reproject + history rejection + clamp +
// moments/variance) and an a-trous wavelet filter, and composites albedo back in.
// Fully in-tree and tunable, with debug views. Needs ray query.
class ReconPathTracer {
 public:
  struct Frame {
    Mat4 inv_view_proj;
    Mat4 view_proj;
    Mat4 prev_view_proj;
    Vec3 camera_pos;
    Vec3 sun_direction;  // travel direction
    f32 sun_intensity = 4.0f;
    Vec3 sun_color{1, 1, 1};
    f32 sun_radius = 0.0f;
    f32 pixel_spread = 0.0f;
    u32 spp = 1;
    u32 frame_index = 0;
    bool reset = false;
    // Tunables.
    f32 current_weight_min = 0.05f;  // floor on current-frame weight (responsiveness)
    u32 max_history = 32;            // history length cap (frames)
    u32 atrous_passes = 4;           // a-trous iterations
    u32 debug_mode = 0;              // 0 final, 1 lighting, 2 history, 3 variance, 4 motion, 5 normal, 6 albedo
    // ReSTIR GI: reservoir-resampled indirect diffuse (temporal + spatial
    // reuse over the initial path samples) instead of integrating the noisy
    // indirect inline. Massively lowers the variance the SVGF chain sees.
    bool restir = true;
    // ReSTIR DI (needs restir): reservoir resampling of the primary direct
    // light over the sun disk AND the frame's dynamic point lights (which the
    // inline path never sampled). One alpha-tested shadow ray per pixel.
    bool restir_di = true;
    GpuBuffer lights;     // host-visible PointLight[], the renderer's frame buffer
    u32 light_count = 0;
  };

  // Filled instead of running the SVGF chain when an external denoiser (DLSS
  // Ray Reconstruction) owns reconstruction: the composed noisy radiance plus
  // the gbuffer guides it consumes. All render-resolution graph handles.
  struct ExternalInputs {
    ResourceHandle color = kInvalidResource;          // albedo/pi*irr + emissive + spec
    ResourceHandle depth = kInvalidResource;          // reversed-inf-z device depth
    ResourceHandle motion = kInvalidResource;         // uv-space current->previous
    ResourceHandle normals_rough = kInvalidResource;  // world normal, roughness in .w
    ResourceHandle diffuse_albedo = kInvalidResource;
    ResourceHandle specular_albedo = kInvalidResource;
  };

  bool Initialize(Device& device, BindingLayoutHandle bindless_layout);
  void Resize(Device& device, Extent2D extent);
  void Destroy(Device& device);

  // Reconstructs the path-traced image into output (scene_color, an hdr storage
  // image), in place of the raster path.
  // With `external` null, reconstructs into output (SVGF). With it set, stops
  // after the gbuffer/restir stages, composes the noisy radiance and fills
  // `external` for the caller's denoiser; output is untouched.
  void AddToGraph(RenderGraph& graph, RayTracingContext& raytracing, u32 tlas_slot,
                  BindingSetHandle bindless_set, TextureView sky_view, SamplerHandle sky_sampler,
                  ResourceHandle output, const Frame& frame, ExternalInputs* external = nullptr);

 private:
  struct PingPong {
    GpuImage image[2];
    ResourceState state[2] = {ResourceState::kUndefined, ResourceState::kUndefined};
  };

  bool CreatePipelines(Device& device, BindingLayoutHandle bindless_layout);
  void CreateBuffers(Device& device, Extent2D extent);
  void DestroyBuffers(Device& device);

  // Reusable per-signal reconstruction (diffuse irradiance and specular both run
  // through these; they share the gbuffer history nr/viewz/matid + motion).
  void RunTemporal(RenderGraph& graph, ResourceHandle noisy, ResourceHandle ac_c,
                   ResourceHandle ac_p, ResourceHandle mo_c, ResourceHandle mo_p, ResourceHandle nr_c,
                   ResourceHandle nr_p, ResourceHandle vz_c, ResourceHandle vz_p, ResourceHandle id_c,
                   ResourceHandle id_p, ResourceHandle motion, ResourceHandle primary_pos, bool spec,
                   const Frame& frame);
  ResourceHandle RunAtrous(RenderGraph& graph, ResourceHandle in, ResourceHandle ping,
                           ResourceHandle pong, ResourceHandle nr_c, ResourceHandle vz_c,
                           ResourceHandle mo_c, u32 passes, bool spec);

  Device* device_ = nullptr;
  Extent2D extent_{};
  u32 spp_ = 1;
  u32 bounces_ = 2;
  // Set when (re)creating the history images: their contents are undefined, so
  // the next frame must drop history even if the caller did not ask for a
  // reset (a mid-session resize otherwise feeds garbage - possibly NaN - into
  // the temporal moments EMA, which never recovers).
  bool history_invalid_ = false;

  // gbuffer (set 0: 7 storage + tlas + sky + restir samples; set 1: bindless)
  PipelineHandle gbuffer_pipeline_;
  PipelineHandle temporal_pipeline_;
  PipelineHandle atrous_pipeline_;
  PipelineHandle composite_pipeline_;
  PipelineHandle restir_temporal_pipeline_;
  PipelineHandle restir_spatial_pipeline_;
  PipelineHandle restir_di_temporal_pipeline_;
  PipelineHandle restir_di_spatial_pipeline_;
  PipelineHandle sky_cdf_pipeline_;

  // Cross-frame ping-pong buffers (indexed by frame_index & 1).
  PingPong accum_;        // rgba16f accumulated diffuse irradiance + variance
  PingPong moments_;      // rgba16f mean, meanSq, variance, historyLen
  PingPong spec_accum_;   // rgba16f accumulated specular + variance
  PingPong spec_moments_; // rgba16f specular moments
  PingPong normal_rough_; // rgba16f normal*0.5+0.5, roughness
  PingPong viewz_;        // r32f
  PingPong matid_;        // r32ui
  // ReSTIR GI reservoirs (pos+W / normal+M / radiance+w_sum). Cross-frame
  // ping-pong like the accumulation history; ~40 B/px per slot.
  PingPong restir_r0_;    // rgba32f sample position, W
  PingPong restir_r1_;    // rgba16f sample normal, M
  PingPong restir_r2_;    // rgba32f sample radiance, w_sum
  // ReSTIR DI reservoirs: r0/r1 sun + point lights, r2/r3 the sky (separate
  // reservoir so both classes stay temporally stable; see the DI shaders).
  PingPong restir_di_r0_;  // rgba32f
  PingPong restir_di_r1_;  // rgba32f
  PingPong restir_di_r2_;  // rgba32f
  PingPong restir_di_r3_;  // rgba32f
  // Sky importance-sampling tables (equirect luminance CDFs), rebuilt in
  // frame by the sky_cdf pass; layout documented in recon_sky_cdf.cs.hlsl.
  GpuBuffer sky_cdf_;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_RECON_PATH_TRACER_H_
