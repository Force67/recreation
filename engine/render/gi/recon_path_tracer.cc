#include "render/gi/recon_path_tracer.h"

#include "core/log.h"
#include "render/gi/raytracing.h"
#include "render/rhi/device.h"
#include "shaders/recon_atrous_cs_hlsl.h"
#include "shaders/recon_composite_cs_hlsl.h"
#include "shaders/recon_gbuffer_cs_hlsl.h"
#include "shaders/recon_restir_di_spatial_cs_hlsl.h"
#include "shaders/recon_restir_di_temporal_cs_hlsl.h"
#include "shaders/recon_restir_spatial_cs_hlsl.h"
#include "shaders/recon_sky_cdf_cs_hlsl.h"
#include "shaders/recon_restir_temporal_cs_hlsl.h"
#include "shaders/recon_temporal_cs_hlsl.h"

namespace rec::render {
namespace {

constexpr Format kIrradiance = Format::kRGBA16Float;
constexpr Format kNormalRough = Format::kRGBA16Float;
constexpr Format kMoments = Format::kRGBA16Float;
constexpr Format kViewZ = Format::kR32Float;
constexpr Format kMotion = Format::kRG16Float;
constexpr Format kMatId = Format::kR32Uint;
constexpr Format kWorldPos = Format::kRGBA32Float;  // restir positions need fp32

// ReSTIR GI tuning (Ouyang et al. 2021 defaults, scaled for 1 initial spp).
constexpr f32 kRestirMMax = 30.0f;         // temporal reservoir age cap
constexpr u32 kRestirSpatialSamples = 5;   // neighbor taps per pixel
constexpr f32 kRestirSpatialRadius = 30.0f;  // neighbor disk radius, pixels
// ReSTIR DI tuning (Bitterli et al. 2020): K uniform light candidates + the
// sun each frame; the age cap scales with the per-frame candidate mass.
constexpr u32 kRestirDiCandidates = 8;
constexpr u32 kRestirDiSkyCandidates = 4;  // sky CDF draws per pixel
constexpr f32 kRestirDiMMax = 20.0f * (kRestirDiCandidates + 1);
constexpr f32 kRestirDiSkyMMax = 20.0f * kRestirDiSkyCandidates;
// Equirect sky CDF grid (must match recon_sky_cdf.cs.hlsl and both DI
// shaders): total + H marginal + W*H row cdf + W*H cell luminance floats.
constexpr u32 kSkyCdfGridW = 128;
constexpr u32 kSkyCdfGridH = 64;
constexpr u64 kSkyCdfFloats = 1 + kSkyCdfGridH + 2 * kSkyCdfGridW * kSkyCdfGridH;
constexpr u32 kRestirDiSpatialSamples = 5;
constexpr f32 kRestirDiSpatialRadius = 30.0f;

struct GbufferPush {
  Mat4 inv_view_proj;
  Mat4 view_proj;
  Mat4 prev_view_proj;
  f32 camera_pos[4];
  f32 sun_direction[4];
  f32 sun_color[4];
  u32 spp;
  f32 pixel_spread;
  u32 frame_index;
  u32 bounces;
};
struct TemporalPush {
  Mat4 prev_view_proj;  // spec: virtual-point reprojection
  f32 camera_pos[4];
  u32 size[2];
  f32 inv_size[2];
  f32 current_weight_min;
  f32 max_history;
  f32 reset;
  u32 spec_mode;
};
struct AtrousPush {
  u32 size[2];
  u32 step_size;
  f32 normal_phi;
  f32 depth_phi;
  f32 luma_phi;
  u32 spec_mode;
  f32 spec_lobe;
};
struct CompositePush {
  u32 size[2];
  u32 debug_mode;
  f32 max_history;
};
struct RestirTemporalPush {
  u32 size[2];
  u32 frame_index;
  f32 m_max;
  f32 reset;
  f32 pad[3];
};
struct RestirSpatialPush {
  u32 size[2];
  u32 frame_index;
  u32 sample_count;
  f32 radius;
  u32 debug;  // 0 off, 1 reservoir M, 2 reservoir W
  f32 pad[2];
};
struct RestirDiTemporalPush {
  f32 sun_direction[4];
  f32 sun_color[4];
  u32 size[2];
  u32 frame_index;
  u32 light_count;
  u32 candidates;
  u32 sky_candidates;
  f32 m_max;
  f32 reset;
  f32 m_max_sky;
  f32 pad[3];
};
struct SkyCdfPush {
  u32 grid[2];
  u32 pad0;
  u32 pad1;
};
struct RestirDiSpatialPush {
  f32 sun_direction[4];
  f32 sun_color[4];
  u32 size[2];
  u32 frame_index;
  u32 light_count;
  u32 sample_count;
  f32 radius;
  f32 pad[2];
};

}  // namespace

bool ReconPathTracer::Initialize(Device& device, BindingLayoutHandle bindless_layout) {
  if (!bindless_layout) return false;
  device_ = &device;
  return CreatePipelines(device, bindless_layout);
}

bool ReconPathTracer::CreatePipelines(Device& device, BindingLayoutHandle bindless_layout) {
  // gbuffer: 7 storage outputs, tlas (7), sky (8), noisy specular out (9),
  // restir initial sample + primary position (10..13); set 1 bindless.
  gbuffer_pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_recon_gbuffer_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kStorageImage},
                          {2, BindingType::kStorageImage},
                          {3, BindingType::kStorageImage},
                          {4, BindingType::kStorageImage},
                          {5, BindingType::kStorageImage},
                          {6, BindingType::kStorageImage},
                          {7, BindingType::kAccelStruct},
                          {8, BindingType::kCombinedTextureSampler},
                          {9, BindingType::kStorageImage},
                          {10, BindingType::kStorageImage},
                          {11, BindingType::kStorageImage},
                          {12, BindingType::kStorageImage},
                          {13, BindingType::kStorageImage},
                          {14, BindingType::kStorageImage},
                          {15, BindingType::kStorageImage},
                          {16, BindingType::kStorageImage}}},
               {.shared = bindless_layout}},
      .push_constant_size = sizeof(GbufferPush),
      .debug_name = "recon_gbuffer",
  });

  restir_temporal_pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_recon_restir_temporal_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kStorageImage},
                          {2, BindingType::kStorageImage},
                          {3, BindingType::kSampledImage},
                          {4, BindingType::kSampledImage},
                          {5, BindingType::kSampledImage},
                          {6, BindingType::kSampledImage},
                          {7, BindingType::kSampledImage},
                          {8, BindingType::kSampledImage},
                          {9, BindingType::kSampledImage},
                          {10, BindingType::kSampledImage},
                          {11, BindingType::kSampledImage},
                          {12, BindingType::kSampledImage},
                          {13, BindingType::kSampledImage},
                          {14, BindingType::kSampledImage},
                          {15, BindingType::kSampledImage},
                          {16, BindingType::kSampledImage}}}},
      .push_constant_size = sizeof(RestirTemporalPush),
      .debug_name = "recon_restir_temporal",
  });

  restir_spatial_pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_recon_restir_spatial_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kSampledImage},
                          {2, BindingType::kSampledImage},
                          {3, BindingType::kSampledImage},
                          {4, BindingType::kSampledImage},
                          {5, BindingType::kSampledImage},
                          {6, BindingType::kSampledImage},
                          {7, BindingType::kSampledImage},
                          {8, BindingType::kSampledImage},
                          {9, BindingType::kAccelStruct},
                          {10, BindingType::kStorageImage},
                          {11, BindingType::kStorageImage},
                          {12, BindingType::kStorageImage}}}},
      .push_constant_size = sizeof(RestirSpatialPush),
      .debug_name = "recon_restir_spatial",
  });

  restir_di_temporal_pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_recon_restir_di_temporal_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kStorageImage},
                          {2, BindingType::kSampledImage},
                          {3, BindingType::kSampledImage},
                          {4, BindingType::kSampledImage},
                          {5, BindingType::kSampledImage},
                          {6, BindingType::kSampledImage},
                          {7, BindingType::kSampledImage},
                          {8, BindingType::kSampledImage},
                          {9, BindingType::kSampledImage},
                          {10, BindingType::kSampledImage},
                          {11, BindingType::kSampledImage},
                          {12, BindingType::kStorageBuffer},
                          {13, BindingType::kCombinedTextureSampler},
                          {14, BindingType::kStorageBuffer},
                          {15, BindingType::kStorageImage},
                          {16, BindingType::kStorageImage},
                          {17, BindingType::kSampledImage},
                          {18, BindingType::kSampledImage}}}},
      .push_constant_size = sizeof(RestirDiTemporalPush),
      .debug_name = "recon_restir_di_temporal",
  });

  sky_cdf_pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_recon_sky_cdf_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kCombinedTextureSampler},
                          {1, BindingType::kStorageBuffer}}}},
      .push_constant_size = sizeof(SkyCdfPush),
      .debug_name = "recon_sky_cdf",
  });

  restir_di_spatial_pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_recon_restir_di_spatial_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kSampledImage},
                          {2, BindingType::kSampledImage},
                          {3, BindingType::kSampledImage},
                          {4, BindingType::kSampledImage},
                          {5, BindingType::kSampledImage},
                          {6, BindingType::kSampledImage},
                          {7, BindingType::kStorageBuffer},
                          {8, BindingType::kAccelStruct},
                          {9, BindingType::kStorageImage},
                          {10, BindingType::kStorageImage},
                          {11, BindingType::kCombinedTextureSampler},
                          {12, BindingType::kStorageBuffer},
                          {13, BindingType::kSampledImage},
                          {14, BindingType::kSampledImage},
                          {15, BindingType::kStorageImage},
                          {16, BindingType::kStorageImage}}},
               {.shared = bindless_layout}},
      .push_constant_size = sizeof(RestirDiSpatialPush),
      .debug_name = "recon_restir_di_spatial",
  });

  temporal_pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_recon_temporal_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kStorageImage},
                          {2, BindingType::kSampledImage},
                          {3, BindingType::kSampledImage},
                          {4, BindingType::kSampledImage},
                          {5, BindingType::kSampledImage},
                          {6, BindingType::kSampledImage},
                          {7, BindingType::kSampledImage},
                          {8, BindingType::kSampledImage},
                          {9, BindingType::kSampledImage},
                          {10, BindingType::kSampledImage},
                          {11, BindingType::kSampledImage},
                          {12, BindingType::kSampledImage}}}},
      .push_constant_size = sizeof(TemporalPush),
      .debug_name = "recon_temporal",
  });

  atrous_pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_recon_atrous_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kSampledImage},
                          {2, BindingType::kSampledImage},
                          {3, BindingType::kSampledImage},
                          {4, BindingType::kSampledImage}}}},
      .push_constant_size = sizeof(AtrousPush),
      .debug_name = "recon_atrous",
  });

  composite_pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_recon_composite_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kSampledImage},
                          {2, BindingType::kSampledImage},
                          {3, BindingType::kSampledImage},
                          {4, BindingType::kSampledImage},
                          {5, BindingType::kSampledImage},
                          {6, BindingType::kSampledImage},
                          {7, BindingType::kSampledImage}}}},
      .push_constant_size = sizeof(CompositePush),
      .debug_name = "recon_composite",
  });

  if (!gbuffer_pipeline_ || !temporal_pipeline_ || !atrous_pipeline_ || !composite_pipeline_ ||
      !restir_temporal_pipeline_ || !restir_spatial_pipeline_ ||
      !restir_di_temporal_pipeline_ || !restir_di_spatial_pipeline_ || !sky_cdf_pipeline_) {
    REC_ERROR("recon path tracer pipeline creation failed");
    return false;
  }
  return true;
}

void ReconPathTracer::CreateBuffers(Device& device, Extent2D extent) {
  extent_ = extent;
  auto make = [&](PingPong& pp, Format fmt) {
    for (u32 i = 0; i < 2; ++i) {
      pp.image[i] =
          device.CreateImage2D(fmt, extent, kTextureUsageSampled | kTextureUsageStorage);
      pp.state[i] = ResourceState::kUndefined;
    }
  };
  make(accum_, kIrradiance);
  make(moments_, kMoments);
  make(spec_accum_, kIrradiance);
  make(spec_moments_, kMoments);
  make(normal_rough_, kNormalRough);
  make(viewz_, kViewZ);
  make(matid_, kMatId);
  make(restir_r0_, kWorldPos);
  make(restir_r1_, kNormalRough);
  make(restir_r2_, kWorldPos);
  make(restir_di_r0_, kWorldPos);
  make(restir_di_r1_, kWorldPos);
  make(restir_di_r2_, kWorldPos);
  make(restir_di_r3_, kWorldPos);
  if (!sky_cdf_) {  // size is resolution-independent; survives Resize
    sky_cdf_ = device.CreateBuffer(kSkyCdfFloats * sizeof(f32), kBufferUsageStorage);
  }
  history_invalid_ = true;

  // Prime every owned image to kGeneral so the first frame's barriers have a
  // defined source state (and reads of the not-yet-written prev slot are legal).
  device.ImmediateSubmit([&](CommandList& cmd) {
    base::Vector<TextureBarrier> barriers;
    for (PingPong* pp : {&accum_, &moments_, &spec_accum_, &spec_moments_, &normal_rough_,
                         &viewz_, &matid_, &restir_r0_, &restir_r1_, &restir_r2_,
                         &restir_di_r0_, &restir_di_r1_, &restir_di_r2_, &restir_di_r3_})
      for (u32 i = 0; i < 2; ++i) {
        barriers.push_back(Transition(pp->image[i], ResourceState::kUndefined,
                                      ResourceState::kGeneral));
        pp->state[i] = ResourceState::kGeneral;
      }
    cmd.TextureBarriers({barriers.data(), barriers.size()});
  });
}

void ReconPathTracer::DestroyBuffers(Device& device) {
  for (PingPong* pp : {&accum_, &moments_, &spec_accum_, &spec_moments_, &normal_rough_,
                       &viewz_, &matid_, &restir_r0_, &restir_r1_, &restir_r2_,
                         &restir_di_r0_, &restir_di_r1_, &restir_di_r2_, &restir_di_r3_})
    for (u32 i = 0; i < 2; ++i)
      if (pp->image[i]) device.DestroyImage(pp->image[i]);
}

void ReconPathTracer::Resize(Device& device, Extent2D extent) {
  if (extent == extent_ && accum_.image[0]) return;
  DestroyBuffers(device);
  CreateBuffers(device, extent);
}

void ReconPathTracer::Destroy(Device& device) {
  DestroyBuffers(device);
  if (sky_cdf_) {
    device.DestroyBuffer(sky_cdf_);
    sky_cdf_ = {};
  }
  for (PipelineHandle* p :
       {&gbuffer_pipeline_, &temporal_pipeline_, &atrous_pipeline_, &composite_pipeline_,
        &restir_temporal_pipeline_, &restir_spatial_pipeline_,
        &restir_di_temporal_pipeline_, &restir_di_spatial_pipeline_, &sky_cdf_pipeline_}) {
    device.DestroyPipeline(*p);
    *p = {};
  }
}

void ReconPathTracer::RunTemporal(RenderGraph& graph, ResourceHandle noisy, ResourceHandle ac_c,
                                  ResourceHandle ac_p, ResourceHandle mo_c, ResourceHandle mo_p,
                                  ResourceHandle nr_c, ResourceHandle nr_p, ResourceHandle vz_c,
                                  ResourceHandle vz_p, ResourceHandle id_c, ResourceHandle id_p,
                                  ResourceHandle motion, ResourceHandle primary_pos, bool spec,
                                  const Frame& frame) {
  graph.AddPass(
      "recon_temporal",
      [&](RenderGraph::PassBuilder& b) {
        b.Write(ac_c, ResourceUsage::kStorageWrite);
        b.Write(mo_c, ResourceUsage::kStorageWrite);
        for (ResourceHandle h :
             {noisy, ac_p, nr_c, nr_p, vz_c, vz_p, motion, id_c, id_p, mo_p, primary_pos})
          b.Read(h, ResourceUsage::kSampledCompute);
      },
      [this, ac_c, mo_c, noisy, ac_p, nr_c, nr_p, vz_c, vz_p, motion, id_c, id_p, mo_p,
       primary_pos, spec, frame](PassContext& ctx) {
        ResourceHandle reads[11] = {noisy, ac_p, nr_c,  nr_p, vz_c,       vz_p,
                                    motion, id_c, id_p, mo_p, primary_pos};
        base::Vector<BindingItem> items;
        items.push_back(Bind::Storage(0, ctx.graph->image(ac_c)));
        items.push_back(Bind::Storage(1, ctx.graph->image(mo_c)));
        for (u32 i = 0; i < 11; ++i) {
          items.push_back(Bind::Sampled(i + 2, ctx.graph->image(reads[i])));
        }

        TemporalPush p{};
        p.prev_view_proj = frame.prev_view_proj;
        p.camera_pos[0] = frame.camera_pos.x;
        p.camera_pos[1] = frame.camera_pos.y;
        p.camera_pos[2] = frame.camera_pos.z;
        p.size[0] = extent_.width; p.size[1] = extent_.height;
        p.inv_size[0] = 1.0f / extent_.width; p.inv_size[1] = 1.0f / extent_.height;
        p.current_weight_min = frame.current_weight_min;
        p.max_history = static_cast<f32>(frame.max_history);
        p.reset = frame.reset ? 1.0f : 0.0f;
        p.spec_mode = spec ? 1u : 0u;
        ctx.cmd->BindPipeline(temporal_pipeline_);
        ctx.cmd->BindTransient(0, {items.data(), items.size()});
        ctx.cmd->Push(p);
        ctx.cmd->Dispatch2D(extent_);
      });
}

ResourceHandle ReconPathTracer::RunAtrous(RenderGraph& graph, ResourceHandle in, ResourceHandle ping,
                                          ResourceHandle pong, ResourceHandle nr_c,
                                          ResourceHandle vz_c, ResourceHandle mo_c, u32 passes,
                                          bool spec) {
  ResourceHandle denoised = in;
  for (u32 i = 0; i < passes; ++i) {
    ResourceHandle out = (i & 1u) ? pong : ping;
    graph.AddPass(
        "recon_atrous",
        [&](RenderGraph::PassBuilder& b) {
          b.Write(out, ResourceUsage::kStorageWrite);
          b.Read(in, ResourceUsage::kSampledCompute);
          b.Read(nr_c, ResourceUsage::kSampledCompute);
          b.Read(vz_c, ResourceUsage::kSampledCompute);
          b.Read(mo_c, ResourceUsage::kSampledCompute);
        },
        [this, in, out, nr_c, vz_c, mo_c, i, spec](PassContext& ctx) {
          AtrousPush p{};
          p.size[0] = extent_.width; p.size[1] = extent_.height;
          p.step_size = 1u << i;
          p.normal_phi = 64.0f;
          p.depth_phi = 80.0f;
          p.luma_phi = 4.0f;
          p.spec_mode = spec ? 1u : 0u;
          p.spec_lobe = 8.0f;  // smooth reflectors keep tight lobes, rough ones filter normally
          ctx.cmd->BindPipeline(atrous_pipeline_);
          ctx.cmd->BindTransient(0, {Bind::Storage(0, ctx.graph->image(out)),
                                     Bind::Sampled(1, ctx.graph->image(in)),
                                     Bind::Sampled(2, ctx.graph->image(nr_c)),
                                     Bind::Sampled(3, ctx.graph->image(vz_c)),
                                     Bind::Sampled(4, ctx.graph->image(mo_c))});
          ctx.cmd->Push(p);
          ctx.cmd->Dispatch2D(extent_);
        });
    in = out;
    denoised = out;
  }
  return denoised;
}

void ReconPathTracer::AddToGraph(RenderGraph& graph, RayTracingContext& raytracing, u32 tlas_slot,
                                 BindingSetHandle bindless_set, TextureView sky_view,
                                 SamplerHandle sky_sampler, ResourceHandle output,
                                 const Frame& original_frame, ExternalInputs* external) {
  // Freshly (re)created history images hold undefined data; force one reset
  // frame so the temporal pass never blends garbage into the moments EMA.
  Frame frame = original_frame;
  frame.reset |= history_invalid_;
  history_invalid_ = false;

  u32 cur = frame.frame_index & 1u;
  u32 prv = 1u - cur;
  auto imp = [&](const char* name, PingPong& pp, u32 i) {
    return graph.ImportImage(name, pp.image[i], &pp.state[i]);
  };

  // Shared gbuffer history (both signals reproject/reject against the same surface).
  ResourceHandle nr_c = imp("recon_nr_c", normal_rough_, cur);
  ResourceHandle nr_p = imp("recon_nr_p", normal_rough_, prv);
  ResourceHandle vz_c = imp("recon_vz_c", viewz_, cur);
  ResourceHandle vz_p = imp("recon_vz_p", viewz_, prv);
  ResourceHandle id_c = imp("recon_id_c", matid_, cur);
  ResourceHandle id_p = imp("recon_id_p", matid_, prv);
  // Diffuse + specular history.
  ResourceHandle ac_c = imp("recon_ac_c", accum_, cur);
  ResourceHandle ac_p = imp("recon_ac_p", accum_, prv);
  ResourceHandle mo_c = imp("recon_mo_c", moments_, cur);
  ResourceHandle mo_p = imp("recon_mo_p", moments_, prv);
  ResourceHandle sac_c = imp("recon_sac_c", spec_accum_, cur);
  ResourceHandle sac_p = imp("recon_sac_p", spec_accum_, prv);
  ResourceHandle smo_c = imp("recon_smo_c", spec_moments_, cur);
  ResourceHandle smo_p = imp("recon_smo_p", spec_moments_, prv);

  auto tex = [&](const char* name, Format fmt) {
    return graph.CreateTexture({.name = name, .format = fmt, .width = extent_.width,
                                .height = extent_.height});
  };
  ResourceHandle spec_noisy = tex("recon_spec_noisy", kIrradiance);
  ResourceHandle motion = tex("recon_motion", kMotion);
  ResourceHandle albedo = tex("recon_albedo", kIrradiance);
  ResourceHandle emissive = tex("recon_emissive", kIrradiance);
  ResourceHandle ping = tex("recon_ping", kIrradiance);
  ResourceHandle pong = tex("recon_pong", kIrradiance);
  ResourceHandle spec_ping = tex("recon_spec_ping", kIrradiance);
  ResourceHandle spec_pong = tex("recon_spec_pong", kIrradiance);
  // ReSTIR initial sample + primary position. Created (and bound) even when
  // restir is off: the gbuffer shader statically references the bindings.
  ResourceHandle s_pos = tex("recon_restir_spos", kWorldPos);
  ResourceHandle s_nrm = tex("recon_restir_snrm", kNormalRough);
  ResourceHandle s_rad = tex("recon_restir_srad", kIrradiance);
  ResourceHandle p_pos = tex("recon_restir_ppos", kWorldPos);
  // With restir the gbuffer emits direct-only irradiance; the spatial stage
  // adds the resampled indirect and produces the noisy input the SVGF
  // temporal pass consumes. Without it the gbuffer output IS the noisy input.
  ResourceHandle gbuf_irr = tex(frame.restir ? "recon_direct" : "recon_noisy", kIrradiance);
  ResourceHandle noisy = frame.restir ? tex("recon_noisy", kIrradiance) : gbuf_irr;
  // DLSS-RR guide outputs; when unused the (format-matching) existing targets
  // alias the bindings so the shader's static references stay valid.
  bool rr = external != nullptr;
  bool di = frame.restir && frame.restir_di;
  ResourceHandle spec_albedo = rr ? tex("recon_rr_spec_albedo", kIrradiance) : albedo;
  ResourceHandle rr_normals = rr ? tex("recon_rr_normals", kNormalRough) : nr_c;
  ResourceHandle rr_depth = rr ? tex("recon_rr_depth", kViewZ) : vz_c;

  // --- 1. gbuffer ---
  graph.AddPass(
      "recon_gbuffer",
      [&](RenderGraph::PassBuilder& b) {
        for (ResourceHandle h : {gbuf_irr, nr_c, vz_c, motion, id_c, albedo, emissive, spec_noisy,
                                 s_pos, s_nrm, s_rad, p_pos})
          b.Write(h, ResourceUsage::kStorageWrite);
        if (rr) {
          for (ResourceHandle h : {spec_albedo, rr_normals, rr_depth})
            b.Write(h, ResourceUsage::kStorageWrite);
        }
      },
      [this, &raytracing, tlas_slot, bindless_set, sky_view, sky_sampler, gbuf_irr, nr_c, vz_c,
       motion, id_c, albedo, emissive, spec_noisy, s_pos, s_nrm, s_rad, p_pos, spec_albedo,
       rr_normals, rr_depth, rr, di, frame](PassContext& ctx) {
        ResourceHandle outs[7] = {gbuf_irr, nr_c, vz_c, motion, id_c, albedo, emissive};
        base::Vector<BindingItem> items;
        for (u32 i = 0; i < 7; ++i) items.push_back(Bind::Storage(i, ctx.graph->image(outs[i])));
        items.push_back(Bind::Accel(7, raytracing.tlas(tlas_slot)));
        items.push_back(Bind::Combined(8, sky_view, sky_sampler));
        items.push_back(Bind::Storage(9, ctx.graph->image(spec_noisy)));
        items.push_back(Bind::Storage(10, ctx.graph->image(s_pos)));
        items.push_back(Bind::Storage(11, ctx.graph->image(s_nrm)));
        items.push_back(Bind::Storage(12, ctx.graph->image(s_rad)));
        items.push_back(Bind::Storage(13, ctx.graph->image(p_pos)));
        items.push_back(Bind::Storage(14, ctx.graph->image(spec_albedo)));
        items.push_back(Bind::Storage(15, ctx.graph->image(rr_normals)));
        items.push_back(Bind::Storage(16, ctx.graph->image(rr_depth)));

        GbufferPush p{};
        p.inv_view_proj = frame.inv_view_proj;
        p.view_proj = frame.view_proj;
        p.prev_view_proj = frame.prev_view_proj;
        p.camera_pos[0] = frame.camera_pos.x; p.camera_pos[1] = frame.camera_pos.y;
        p.camera_pos[2] = frame.camera_pos.z;
        Vec3 sun = Normalize(frame.sun_direction);
        p.sun_direction[0] = sun.x; p.sun_direction[1] = sun.y; p.sun_direction[2] = sun.z;
        p.sun_direction[3] = frame.sun_intensity;
        p.sun_color[0] = frame.sun_color.x; p.sun_color[1] = frame.sun_color.y;
        p.sun_color[2] = frame.sun_color.z; p.sun_color[3] = frame.sun_radius;
        p.spp = frame.spp < 1 ? 1u : frame.spp;
        p.pixel_spread = frame.pixel_spread;
        p.frame_index = frame.frame_index;
        // bits 0..7 bounce count, bit 8 restir, bit 9 rr guides (the push
        // block is at the 256 B cap).
        p.bounces = (bounces_ & 0xffu) | (frame.restir ? 0x100u : 0u) | (rr ? 0x200u : 0u) |
                    (di ? 0x400u : 0u);
        ctx.cmd->BindPipeline(gbuffer_pipeline_);
        ctx.cmd->BindTransient(0, {items.data(), items.size()});
        ctx.cmd->BindSet(1, bindless_set);
        ctx.cmd->Push(p);
        ctx.cmd->Dispatch2D(extent_);
      });

  // --- 1b. ReSTIR GI: temporal + spatial reservoir resampling of the initial
  // indirect samples, then shading into the noisy irradiance the SVGF chain
  // consumes. Reservoir history ping-pongs with the frame parity like the
  // accumulation targets; the temporal output (pre-spatial) is what the next
  // frame reuses, which keeps spatial correlation out of the history.
  // --- 1a. ReSTIR DI: light-sample reservoirs (sun disk + dynamic point
  // lights) replace the gbuffer's inline sun sampling for the primary direct
  // term. Temporal writes transient reservoirs, spatial merges + traces the
  // one shadow ray, shades into direct_irr and persists the merged reservoir
  // for next frame (same feedback layout as the GI reservoirs below).
  ResourceHandle direct_irr = gbuf_irr;
  if (di) {
    direct_irr = tex("recon_di_direct", kIrradiance);

    // Sky sampling tables: one 32-thread workgroup over the 64x32 equirect
    // grid, every frame (sunrise-cheap, avoids sun-change tracking). The
    // manual barrier covers the buffer hazard; graph pass order covers
    // execution (same pattern as the exposure histogram).
    graph.AddPass(
        "recon_sky_cdf",
        [](RenderGraph::PassBuilder&) {},
        [this, sky_view, sky_sampler](PassContext& ctx) {
          SkyCdfPush p{};
          p.grid[0] = kSkyCdfGridW;
          p.grid[1] = kSkyCdfGridH;
          ctx.cmd->BindPipeline(sky_cdf_pipeline_);
          ctx.cmd->BindTransient(
              0, {Bind::Combined(0, sky_view, sky_sampler),
                  Bind::StorageBuffer(1, sky_cdf_, 0, sky_cdf_.size)});
          ctx.cmd->Push(p);
          ctx.cmd->Dispatch(1, 1, 1);
          ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);
        });
    ResourceHandle d0_t = tex("recon_di_rsv0_t", kWorldPos);
    ResourceHandle d1_t = tex("recon_di_rsv1_t", kWorldPos);
    ResourceHandle d2_t = tex("recon_di_rsv2_t", kWorldPos);
    ResourceHandle d3_t = tex("recon_di_rsv3_t", kWorldPos);
    ResourceHandle d0_c = imp("recon_di_rsv0_c", restir_di_r0_, cur);
    ResourceHandle d0_p = imp("recon_di_rsv0_p", restir_di_r0_, prv);
    ResourceHandle d1_c = imp("recon_di_rsv1_c", restir_di_r1_, cur);
    ResourceHandle d1_p = imp("recon_di_rsv1_p", restir_di_r1_, prv);
    ResourceHandle d2_c = imp("recon_di_rsv2_c", restir_di_r2_, cur);
    ResourceHandle d2_p = imp("recon_di_rsv2_p", restir_di_r2_, prv);
    ResourceHandle d3_c = imp("recon_di_rsv3_c", restir_di_r3_, cur);
    ResourceHandle d3_p = imp("recon_di_rsv3_p", restir_di_r3_, prv);

    graph.AddPass(
        "recon_restir_di_temporal",
        [&](RenderGraph::PassBuilder& b) {
          for (ResourceHandle h : {d0_t, d1_t, d2_t, d3_t})
            b.Write(h, ResourceUsage::kStorageWrite);
          for (ResourceHandle h :
               {p_pos, nr_c, nr_p, vz_c, vz_p, id_c, id_p, motion, d0_p, d1_p, d2_p, d3_p})
            b.Read(h, ResourceUsage::kSampledCompute);
        },
        [this, d0_t, d1_t, d2_t, d3_t, p_pos, nr_c, nr_p, vz_c, vz_p, id_c, id_p, motion, d0_p,
         d1_p, d2_p, d3_p, sky_view, sky_sampler, frame](PassContext& ctx) {
          ResourceHandle reads[10] = {p_pos, nr_c, nr_p, vz_c, vz_p,
                                      id_c,  id_p, motion, d0_p, d1_p};
          base::Vector<BindingItem> items;
          items.push_back(Bind::Storage(0, ctx.graph->image(d0_t)));
          items.push_back(Bind::Storage(1, ctx.graph->image(d1_t)));
          for (u32 i = 0; i < 10; ++i)
            items.push_back(Bind::Sampled(i + 2, ctx.graph->image(reads[i])));
          items.push_back(Bind::StorageBuffer(12, frame.lights));
          items.push_back(Bind::Combined(13, sky_view, sky_sampler));
          items.push_back(Bind::StorageBuffer(14, sky_cdf_, 0, sky_cdf_.size));
          items.push_back(Bind::Storage(15, ctx.graph->image(d2_t)));
          items.push_back(Bind::Storage(16, ctx.graph->image(d3_t)));
          items.push_back(Bind::Sampled(17, ctx.graph->image(d2_p)));
          items.push_back(Bind::Sampled(18, ctx.graph->image(d3_p)));

          RestirDiTemporalPush p{};
          Vec3 sun = Normalize(frame.sun_direction);
          p.sun_direction[0] = sun.x; p.sun_direction[1] = sun.y; p.sun_direction[2] = sun.z;
          p.sun_direction[3] = frame.sun_intensity;
          p.sun_color[0] = frame.sun_color.x; p.sun_color[1] = frame.sun_color.y;
          p.sun_color[2] = frame.sun_color.z; p.sun_color[3] = frame.sun_radius;
          p.size[0] = extent_.width; p.size[1] = extent_.height;
          p.frame_index = frame.frame_index;
          p.light_count = frame.light_count;
          p.candidates = kRestirDiCandidates;
          p.sky_candidates = kRestirDiSkyCandidates;
          p.m_max = kRestirDiMMax;
          p.reset = frame.reset ? 1.0f : 0.0f;
          p.m_max_sky = kRestirDiSkyMMax;
          ctx.cmd->BindPipeline(restir_di_temporal_pipeline_);
          ctx.cmd->BindTransient(0, {items.data(), items.size()});
          ctx.cmd->Push(p);
          ctx.cmd->Dispatch2D(extent_);
        });

    graph.AddPass(
        "recon_restir_di_spatial",
        [&](RenderGraph::PassBuilder& b) {
          for (ResourceHandle h : {direct_irr, d0_c, d1_c, d2_c, d3_c})
            b.Write(h, ResourceUsage::kStorageWrite);
          for (ResourceHandle h : {d0_t, d1_t, d2_t, d3_t, p_pos, nr_c, vz_c, id_c})
            b.Read(h, ResourceUsage::kSampledCompute);
        },
        [this, &raytracing, tlas_slot, bindless_set, direct_irr, d0_t, d1_t, d2_t, d3_t, d0_c,
         d1_c, d2_c, d3_c, p_pos, nr_c, vz_c, id_c, sky_view, sky_sampler,
         frame](PassContext& ctx) {
          ResourceHandle reads[6] = {d0_t, d1_t, p_pos, nr_c, vz_c, id_c};
          base::Vector<BindingItem> items;
          items.push_back(Bind::Storage(0, ctx.graph->image(direct_irr)));
          for (u32 i = 0; i < 6; ++i)
            items.push_back(Bind::Sampled(i + 1, ctx.graph->image(reads[i])));
          items.push_back(Bind::StorageBuffer(7, frame.lights));
          items.push_back(Bind::Accel(8, raytracing.tlas(tlas_slot)));
          items.push_back(Bind::Storage(9, ctx.graph->image(d0_c)));
          items.push_back(Bind::Storage(10, ctx.graph->image(d1_c)));
          items.push_back(Bind::Combined(11, sky_view, sky_sampler));
          items.push_back(Bind::StorageBuffer(12, sky_cdf_, 0, sky_cdf_.size));
          items.push_back(Bind::Sampled(13, ctx.graph->image(d2_t)));
          items.push_back(Bind::Sampled(14, ctx.graph->image(d3_t)));
          items.push_back(Bind::Storage(15, ctx.graph->image(d2_c)));
          items.push_back(Bind::Storage(16, ctx.graph->image(d3_c)));

          RestirDiSpatialPush p{};
          Vec3 sun = Normalize(frame.sun_direction);
          p.sun_direction[0] = sun.x; p.sun_direction[1] = sun.y; p.sun_direction[2] = sun.z;
          p.sun_direction[3] = frame.sun_intensity;
          p.sun_color[0] = frame.sun_color.x; p.sun_color[1] = frame.sun_color.y;
          p.sun_color[2] = frame.sun_color.z; p.sun_color[3] = frame.sun_radius;
          p.size[0] = extent_.width; p.size[1] = extent_.height;
          p.frame_index = frame.frame_index;
          p.light_count = frame.light_count;
          p.sample_count = kRestirDiSpatialSamples;
          p.radius = kRestirDiSpatialRadius;
          ctx.cmd->BindPipeline(restir_di_spatial_pipeline_);
          ctx.cmd->BindTransient(0, {items.data(), items.size()});
          ctx.cmd->BindSet(1, bindless_set);
          ctx.cmd->Push(p);
          ctx.cmd->Dispatch2D(extent_);
        });
  }

  if (frame.restir) {
    // Temporal resamples into TRANSIENT reservoirs; spatial merges neighbors
    // from those and writes the persistent slot, so next frame's temporal
    // stage reuses the spatially compounded history (paper feedback loop).
    ResourceHandle r0_t = tex("recon_rsv0_t", kWorldPos);
    ResourceHandle r1_t = tex("recon_rsv1_t", kNormalRough);
    ResourceHandle r2_t = tex("recon_rsv2_t", kWorldPos);
    ResourceHandle r0_c = imp("recon_rsv0_c", restir_r0_, cur);
    ResourceHandle r0_p = imp("recon_rsv0_p", restir_r0_, prv);
    ResourceHandle r1_c = imp("recon_rsv1_c", restir_r1_, cur);
    ResourceHandle r1_p = imp("recon_rsv1_p", restir_r1_, prv);
    ResourceHandle r2_c = imp("recon_rsv2_c", restir_r2_, cur);
    ResourceHandle r2_p = imp("recon_rsv2_p", restir_r2_, prv);

    graph.AddPass(
        "recon_restir_temporal",
        [&](RenderGraph::PassBuilder& b) {
          for (ResourceHandle h : {r0_t, r1_t, r2_t}) b.Write(h, ResourceUsage::kStorageWrite);
          for (ResourceHandle h :
               {s_pos, s_nrm, s_rad, p_pos, nr_c, nr_p, vz_c, vz_p, id_c, id_p, motion, r0_p,
                r1_p, r2_p})
            b.Read(h, ResourceUsage::kSampledCompute);
        },
        [this, r0_t, r1_t, r2_t, s_pos, s_nrm, s_rad, p_pos, nr_c, nr_p, vz_c, vz_p, id_c, id_p,
         motion, r0_p, r1_p, r2_p, frame](PassContext& ctx) {
          ResourceHandle reads[14] = {s_pos, s_nrm, s_rad, p_pos, nr_c, nr_p, vz_c,
                                      vz_p, id_c, id_p, motion, r0_p, r1_p, r2_p};
          base::Vector<BindingItem> items;
          items.push_back(Bind::Storage(0, ctx.graph->image(r0_t)));
          items.push_back(Bind::Storage(1, ctx.graph->image(r1_t)));
          items.push_back(Bind::Storage(2, ctx.graph->image(r2_t)));
          for (u32 i = 0; i < 14; ++i)
            items.push_back(Bind::Sampled(i + 3, ctx.graph->image(reads[i])));

          RestirTemporalPush p{};
          p.size[0] = extent_.width; p.size[1] = extent_.height;
          p.frame_index = frame.frame_index;
          p.m_max = kRestirMMax;
          p.reset = frame.reset ? 1.0f : 0.0f;
          ctx.cmd->BindPipeline(restir_temporal_pipeline_);
          ctx.cmd->BindTransient(0, {items.data(), items.size()});
          ctx.cmd->Push(p);
          ctx.cmd->Dispatch2D(extent_);
        });

    graph.AddPass(
        "recon_restir_spatial",
        [&](RenderGraph::PassBuilder& b) {
          b.Write(noisy, ResourceUsage::kStorageWrite);
          for (ResourceHandle h : {r0_c, r1_c, r2_c}) b.Write(h, ResourceUsage::kStorageWrite);
          for (ResourceHandle h : {r0_t, r1_t, r2_t, p_pos, nr_c, vz_c, id_c, direct_irr})
            b.Read(h, ResourceUsage::kSampledCompute);
        },
        [this, &raytracing, tlas_slot, noisy, r0_t, r1_t, r2_t, r0_c, r1_c, r2_c, p_pos, nr_c,
         vz_c, id_c, direct_irr, frame](PassContext& ctx) {
          ResourceHandle reads[8] = {r0_t, r1_t, r2_t, p_pos, nr_c, vz_c, id_c, direct_irr};
          base::Vector<BindingItem> items;
          items.push_back(Bind::Storage(0, ctx.graph->image(noisy)));
          for (u32 i = 0; i < 8; ++i)
            items.push_back(Bind::Sampled(i + 1, ctx.graph->image(reads[i])));
          items.push_back(Bind::Accel(9, raytracing.tlas(tlas_slot)));
          items.push_back(Bind::Storage(10, ctx.graph->image(r0_c)));
          items.push_back(Bind::Storage(11, ctx.graph->image(r1_c)));
          items.push_back(Bind::Storage(12, ctx.graph->image(r2_c)));

          RestirSpatialPush p{};
          p.size[0] = extent_.width; p.size[1] = extent_.height;
          p.frame_index = frame.frame_index;
          p.sample_count = kRestirSpatialSamples;
          p.radius = kRestirSpatialRadius;
          // debug 8/9 route the reservoir M / W heatmap through the noisy
          // channel (the composite's mode-1 lighting view displays it).
          p.debug = frame.debug_mode >= 8 ? frame.debug_mode - 7 : 0;
          ctx.cmd->BindPipeline(restir_spatial_pipeline_);
          ctx.cmd->BindTransient(0, {items.data(), items.size()});
          ctx.cmd->Push(p);
          ctx.cmd->Dispatch2D(extent_);
        });
  }

  // --- external denoiser (DLSS-RR): compose the NOISY radiance with the
  // regular composite pipeline (mode 0 is exactly albedo/pi*e + emissive +
  // spec) and hand the guides back; SVGF stays out of the loop. ---
  if (external) {
    ResourceHandle rr_color = tex("recon_rr_color", kIrradiance);
    graph.AddPass(
        "recon_rr_compose",
        [&](RenderGraph::PassBuilder& b) {
          b.Write(rr_color, ResourceUsage::kStorageWrite);
          for (ResourceHandle h : {albedo, noisy, emissive, mo_c, nr_c, motion, spec_noisy})
            b.Read(h, ResourceUsage::kSampledCompute);
        },
        [this, rr_color, albedo, noisy, emissive, mo_c, nr_c, motion,
         spec_noisy](PassContext& ctx) {
          ResourceHandle reads[7] = {albedo, noisy, emissive, mo_c, nr_c, motion, spec_noisy};
          base::Vector<BindingItem> items;
          items.push_back(Bind::Storage(0, ctx.graph->image(rr_color)));
          for (u32 i = 0; i < 7; ++i)
            items.push_back(Bind::Sampled(i + 1, ctx.graph->image(reads[i])));
          CompositePush p{};
          p.size[0] = extent_.width; p.size[1] = extent_.height;
          p.debug_mode = 0;
          p.max_history = 1.0f;
          ctx.cmd->BindPipeline(composite_pipeline_);
          ctx.cmd->BindTransient(0, {items.data(), items.size()});
          ctx.cmd->Push(p);
          ctx.cmd->Dispatch2D(extent_);
        });
    external->color = rr_color;
    external->depth = rr_depth;
    external->motion = motion;
    external->normals_rough = rr_normals;
    external->diffuse_albedo = albedo;
    external->specular_albedo = spec_albedo;
    return;
  }

  // --- 2. temporal accumulation (diffuse + specular share the gbuffer history) ---
  RunTemporal(graph, noisy, ac_c, ac_p, mo_c, mo_p, nr_c, nr_p, vz_c, vz_p, id_c, id_p, motion,
              p_pos, /*spec=*/false, frame);
  RunTemporal(graph, spec_noisy, sac_c, sac_p, smo_c, smo_p, nr_c, nr_p, vz_c, vz_p, id_c, id_p,
              motion, p_pos, /*spec=*/true, frame);

  // --- 3. a-trous (N passes, ping-pong) for each signal ---
  u32 passes = frame.atrous_passes == 0 ? 1u : frame.atrous_passes;
  ResourceHandle denoised = RunAtrous(graph, ac_c, ping, pong, nr_c, vz_c, mo_c, passes, false);
  ResourceHandle spec_denoised =
      RunAtrous(graph, sac_c, spec_ping, spec_pong, nr_c, vz_c, smo_c, passes, true);

  // --- 4. composite ---
  graph.AddPass(
      "recon_composite",
      [&](RenderGraph::PassBuilder& b) {
        b.Write(output, ResourceUsage::kStorageWrite);
        for (ResourceHandle h : {albedo, denoised, emissive, mo_c, nr_c, motion, spec_denoised})
          b.Read(h, ResourceUsage::kSampledCompute);
      },
      [this, output, albedo, denoised, emissive, mo_c, nr_c, motion, spec_denoised,
       frame](PassContext& ctx) {
        ResourceHandle reads[7] = {albedo, denoised, emissive, mo_c, nr_c, motion, spec_denoised};
        base::Vector<BindingItem> items;
        items.push_back(Bind::Storage(0, ctx.graph->image(output)));
        for (u32 i = 0; i < 7; ++i) {
          items.push_back(Bind::Sampled(i + 1, ctx.graph->image(reads[i])));
        }

        CompositePush p{};
        p.size[0] = extent_.width; p.size[1] = extent_.height;
        p.debug_mode = frame.debug_mode;
        p.max_history = static_cast<f32>(frame.max_history);
        ctx.cmd->BindPipeline(composite_pipeline_);
        ctx.cmd->BindTransient(0, {items.data(), items.size()});
        ctx.cmd->Push(p);
        ctx.cmd->Dispatch2D(extent_);
      });
}

}  // namespace rec::render
