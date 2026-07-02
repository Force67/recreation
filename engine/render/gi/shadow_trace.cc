#include "render/gi/shadow_trace.h"

#include <cmath>

#include "core/log.h"
#include "render/gi/denoiser_nrd.h"
#include "render/gi/raytracing.h"
#include "render/rhi/device.h"

// shadow_trace.cs pulls in NRD.hlsli for the SIGMA penumbra packing, so it only
// compiles (and this pass only exists) when NRD is built in.
#if defined(RECREATION_HAS_NRD)
#include "shaders/shadow_trace_cs_hlsl.h"

namespace rec::render {
namespace {

struct ShadowTracePush {
  Mat4 inv_view_proj;
  f32 to_light_x;
  f32 to_light_y;
  f32 to_light_z;
  f32 near_plane;
  f32 inv_size[2];
  f32 tan_angular_radius;
  f32 max_distance;
  f32 jitter[2];
};

}  // namespace

bool ShadowTracePass::Initialize(Device& device) {
  // 0: penumbra out, 1: depth, 2: tlas.
  pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_shadow_trace_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kSampledImage},
                          {2, BindingType::kAccelStruct}}}},
      .push_constant_size = sizeof(ShadowTracePush),
      .debug_name = "shadow_trace",
  });
  if (!pipeline_) {
    REC_ERROR("shadow trace pipeline creation failed");
    return false;
  }
  return true;
}

void ShadowTracePass::Destroy(Device& device) {
  device.DestroyPipeline(pipeline_);
  pipeline_ = {};
}

ResourceHandle ShadowTracePass::AddToGraph(RenderGraph& graph, RayTracingContext& raytracing,
                                           u32 tlas_slot, ResourceHandle depth,
                                           const Mat4& inv_view_proj, const Vec3& sun_direction,
                                           f32 near_plane, f32 angular_radius, f32 jitter_x,
                                           f32 jitter_y) {
  ResourceHandle penumbra =
      graph.CreateTexture({.name = "shadow_penumbra", .format = NrdDenoiser::kPenumbraFormat,
                           .width = extent_.width, .height = extent_.height});

  Vec3 to_light = Normalize(sun_direction * -1.0f);
  graph.AddPass(
      "shadow_trace",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(depth, ResourceUsage::kSampledCompute);
        builder.Write(penumbra, ResourceUsage::kStorageWrite);
      },
      [this, &raytracing, tlas_slot, depth, penumbra, inv_view_proj, to_light, near_plane,
       angular_radius, jitter_x, jitter_y](PassContext& ctx) {
        ShadowTracePush push{};
        push.inv_view_proj = inv_view_proj;
        push.to_light_x = to_light.x;
        push.to_light_y = to_light.y;
        push.to_light_z = to_light.z;
        push.near_plane = near_plane;
        push.inv_size[0] = 1.0f / static_cast<f32>(extent_.width);
        push.inv_size[1] = 1.0f / static_cast<f32>(extent_.height);
        push.tan_angular_radius = std::tan(angular_radius > 0.0f ? angular_radius : 0.0045f);
        push.max_distance = 1000.0f;
        push.jitter[0] = jitter_x;
        push.jitter[1] = jitter_y;

        ctx.cmd->BindPipeline(pipeline_);
        ctx.cmd->BindTransient(0, {Bind::Storage(0, ctx.graph->image(penumbra)),
                                   Bind::Sampled(1, ctx.graph->image(depth)),
                                   Bind::Accel(2, raytracing.tlas(tlas_slot))});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D(extent_);
      });
  return penumbra;
}

}  // namespace rec::render

#endif  // RECREATION_HAS_NRD
