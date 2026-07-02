#include "render/gi/path_tracer.h"

#include "core/log.h"
#include "render/gi/raytracing.h"
#include "render/rhi/device.h"
#include "shaders/pathtrace_cs_hlsl.h"
#if defined(RECREATION_HAS_NRD)
#include "shaders/pathtrace_composite_cs_hlsl.h"
#include "shaders/pathtrace_gbuffer_cs_hlsl.h"
#endif

namespace rec::render {
namespace {

struct PathPush {
  Mat4 inv_view_proj;
  f32 camera_pos[4];
  f32 sun_direction[4];
  f32 sun_color[4];
  u32 size[2];
  u32 frame_index;
  u32 sample_base;
  u32 spp;
  u32 bounces;
  u32 reset;
  u32 pad;
};

constexpr Format kAccumFormat = Format::kRGBA32Float;

#if defined(RECREATION_HAS_NRD)
// Matches PathGbufferPush in pathtrace_gbuffer.cs.hlsl.
struct PathGbufferPush {
  Mat4 inv_view_proj;
  Mat4 view_proj;
  Mat4 prev_view_proj;
  f32 camera_pos[4];
  f32 sun_direction[4];
  f32 sun_color[4];
  u32 spp;  // lighting samples per pixel (size derived from GetDimensions)
  f32 pixel_spread;  // ray-cone spread angle (radians/pixel) for texture mip lod
  u32 frame_index;
  u32 bounces;
};

struct CompositePush {
  u32 size[2];
};
#endif  // RECREATION_HAS_NRD

}  // namespace

bool PathTracer::Initialize(Device& device, BindingLayoutHandle bindless_layout) {
  if (!bindless_layout) return false;

  pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_pathtrace_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kStorageImage},
                          {2, BindingType::kAccelStruct},
                          {3, BindingType::kCombinedTextureSampler}}},
               {.shared = bindless_layout}},
      .push_constant_size = sizeof(PathPush),
      .debug_name = "pathtrace",
  });
  if (!pipeline_) {
    REC_ERROR("path tracer pipeline creation failed");
    return false;
  }

#if defined(RECREATION_HAS_NRD)
  // Denoised gbuffer: 6 NRD-input storage images (0..5), tlas (6), sky (7).
  gbuffer_pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_pathtrace_gbuffer_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kStorageImage},
                          {2, BindingType::kStorageImage},
                          {3, BindingType::kStorageImage},
                          {4, BindingType::kStorageImage},
                          {5, BindingType::kStorageImage},
                          {6, BindingType::kAccelStruct},
                          {7, BindingType::kCombinedTextureSampler}}},
               {.shared = bindless_layout}},
      .push_constant_size = sizeof(PathGbufferPush),
      .debug_name = "pathtrace_gbuffer",
  });
  if (!gbuffer_pipeline_) {
    REC_ERROR("path tracer gbuffer pipeline creation failed");
    return false;
  }

  // Composite: output storage (0) + denoised/albedo/background sampled (1..3).
  composite_pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_pathtrace_composite_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kSampledImage},
                          {2, BindingType::kSampledImage},
                          {3, BindingType::kSampledImage}}}},
      .push_constant_size = sizeof(CompositePush),
      .debug_name = "pathtrace_composite",
  });
  if (!composite_pipeline_) {
    REC_ERROR("path tracer composite pipeline creation failed");
    return false;
  }
#endif  // RECREATION_HAS_NRD
  return true;
}

void PathTracer::Resize(Device& device, Extent2D extent) {
  if (extent.width == extent_.width && extent.height == extent_.height && accum_) return;
  if (accum_) device.DestroyImage(accum_);
  extent_ = extent;
  accum_ = device.CreateImage2D(kAccumFormat, extent, kTextureUsageStorage);
  accum_state_ = ResourceState::kUndefined;
  accumulated_samples_ = 0;
  if (!accum_) return;

  // The graph imports the buffer in GENERAL; transition it once up front.
  device.ImmediateSubmit([this](CommandList& cmd) {
    cmd.Barrier(Transition(accum_, ResourceState::kUndefined, ResourceState::kGeneral));
  });
  accum_state_ = ResourceState::kGeneral;
}

void PathTracer::Destroy(Device& device) {
  if (accum_) device.DestroyImage(accum_);
  for (PipelineHandle* p : {&pipeline_, &gbuffer_pipeline_, &composite_pipeline_}) {
    device.DestroyPipeline(*p);
    *p = {};
  }
}

void PathTracer::AddToGraph(RenderGraph& graph, RayTracingContext& raytracing, u32 tlas_slot,
                            BindingSetHandle bindless_set, TextureView sky_view,
                            SamplerHandle sky_sampler, ResourceHandle output, const Frame& frame) {
  if (frame.reset) accumulated_samples_ = 0;
  u32 sample_base = accumulated_samples_;
  accumulated_samples_ += spp_;

  ResourceHandle accum = graph.ImportImage("pt_accum", accum_, &accum_state_);
  graph.AddPass(
      "pathtrace",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Write(accum, ResourceUsage::kStorageWrite);
        builder.Write(output, ResourceUsage::kStorageWrite);
      },
      [this, &raytracing, tlas_slot, bindless_set, sky_view, sky_sampler, output, accum,
       frame, sample_base](PassContext& ctx) {
        PathPush push{};
        push.inv_view_proj = frame.inv_view_proj;
        push.camera_pos[0] = frame.camera_pos.x;
        push.camera_pos[1] = frame.camera_pos.y;
        push.camera_pos[2] = frame.camera_pos.z;
        Vec3 sun = Normalize(frame.sun_direction);
        push.sun_direction[0] = sun.x;
        push.sun_direction[1] = sun.y;
        push.sun_direction[2] = sun.z;
        push.sun_direction[3] = frame.sun_intensity;
        push.sun_color[0] = frame.sun_color.x;
        push.sun_color[1] = frame.sun_color.y;
        push.sun_color[2] = frame.sun_color.z;
        push.sun_color[3] = frame.sun_radius;
        push.size[0] = extent_.width;
        push.size[1] = extent_.height;
        push.frame_index = frame.frame_index;
        push.sample_base = sample_base;
        push.spp = spp_;
        push.bounces = bounces_;
        push.reset = sample_base == 0 ? 1u : 0u;

        ctx.cmd->BindPipeline(pipeline_);
        ctx.cmd->BindTransient(0, {Bind::Storage(0, ctx.graph->image(output)),
                                   Bind::Storage(1, ctx.graph->image(accum)),
                                   Bind::Accel(2, raytracing.tlas(tlas_slot)),
                                   Bind::Combined(3, sky_view, sky_sampler)});
        ctx.cmd->BindSet(1, bindless_set);
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D(extent_);
      });
}

#if defined(RECREATION_HAS_NRD)
void PathTracer::AddGbufferPass(RenderGraph& graph, RayTracingContext& raytracing, u32 tlas_slot,
                                BindingSetHandle bindless_set, TextureView sky_view,
                                SamplerHandle sky_sampler, const GbufferTargets& t,
                                const Frame& frame) {
  graph.AddPass(
      "pathtrace_gbuffer",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Write(t.radiance_hitdist, ResourceUsage::kStorageWrite);
        builder.Write(t.normal_roughness, ResourceUsage::kStorageWrite);
        builder.Write(t.viewz, ResourceUsage::kStorageWrite);
        builder.Write(t.motion, ResourceUsage::kStorageWrite);
        builder.Write(t.albedo, ResourceUsage::kStorageWrite);
        builder.Write(t.background, ResourceUsage::kStorageWrite);
      },
      [this, &raytracing, tlas_slot, bindless_set, sky_view, sky_sampler, t,
       frame](PassContext& ctx) {
        ResourceHandle handles[6] = {t.radiance_hitdist, t.normal_roughness, t.viewz,
                                     t.motion, t.albedo, t.background};
        base::Vector<BindingItem> items;
        for (u32 i = 0; i < 6; ++i) items.push_back(Bind::Storage(i, ctx.graph->image(handles[i])));
        items.push_back(Bind::Accel(6, raytracing.tlas(tlas_slot)));
        items.push_back(Bind::Combined(7, sky_view, sky_sampler));

        PathGbufferPush push{};
        push.inv_view_proj = frame.inv_view_proj;
        push.view_proj = frame.view_proj;
        push.prev_view_proj = frame.prev_view_proj;
        push.camera_pos[0] = frame.camera_pos.x;
        push.camera_pos[1] = frame.camera_pos.y;
        push.camera_pos[2] = frame.camera_pos.z;
        Vec3 sun = Normalize(frame.sun_direction);
        push.sun_direction[0] = sun.x;
        push.sun_direction[1] = sun.y;
        push.sun_direction[2] = sun.z;
        push.sun_direction[3] = frame.sun_intensity;
        push.sun_color[0] = frame.sun_color.x;
        push.sun_color[1] = frame.sun_color.y;
        push.sun_color[2] = frame.sun_color.z;
        push.sun_color[3] = frame.sun_radius;
        push.spp = frame.spp < 1 ? 1u : frame.spp;
        push.pixel_spread = frame.pixel_spread;
        push.frame_index = frame.frame_index;
        push.bounces = bounces_;

        ctx.cmd->BindPipeline(gbuffer_pipeline_);
        ctx.cmd->BindTransient(0, {items.data(), items.size()});
        ctx.cmd->BindSet(1, bindless_set);
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D(extent_);
      });
}

void PathTracer::AddCompositePass(RenderGraph& graph, ResourceHandle denoised, ResourceHandle albedo,
                                  ResourceHandle background, ResourceHandle output) {
  graph.AddPass(
      "pathtrace_composite",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(denoised, ResourceUsage::kSampledCompute);
        builder.Read(albedo, ResourceUsage::kSampledCompute);
        builder.Read(background, ResourceUsage::kSampledCompute);
        builder.Write(output, ResourceUsage::kStorageWrite);
      },
      [this, denoised, albedo, background, output](PassContext& ctx) {
        CompositePush push{{extent_.width, extent_.height}};
        ctx.cmd->BindPipeline(composite_pipeline_);
        ctx.cmd->BindTransient(0, {Bind::Storage(0, ctx.graph->image(output)),
                                   Bind::Sampled(1, ctx.graph->image(denoised)),
                                   Bind::Sampled(2, ctx.graph->image(albedo)),
                                   Bind::Sampled(3, ctx.graph->image(background))});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D(extent_);
      });
}
#endif  // RECREATION_HAS_NRD

}  // namespace rec::render
