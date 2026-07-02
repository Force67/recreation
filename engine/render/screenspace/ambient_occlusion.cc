#include "render/screenspace/ambient_occlusion.h"

#include "core/log.h"
#include "render/gi/raytracing.h"
#include "render/rhi/device.h"
#include "shaders/rtao_cs_hlsl.h"

namespace rec::render {
namespace {

struct TracePush {
  Mat4 inv_view_proj;
  f32 inv_size[2];
  f32 radius;
  f32 near_plane;
  f32 hit_a;
  f32 hit_b;
  f32 hit_c;
  f32 frame_index;
  u32 ray_count;
};

}  // namespace

bool RtaoPass::Initialize(Device& device) {
  pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_rtao_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kSampledImage},
                          {2, BindingType::kSampledImage},
                          {3, BindingType::kAccelStruct}}}},
      .push_constant_size = sizeof(TracePush),
      .debug_name = "rtao_trace",
  });
  if (!pipeline_) {
    REC_ERROR("rtao pipeline creation failed");
    return false;
  }
  return true;
}

void RtaoPass::Destroy(Device& device) {
  device.DestroyPipeline(pipeline_);
  pipeline_ = {};
}

ResourceHandle RtaoPass::AddToGraph(RenderGraph& graph, RayTracingContext& raytracing, u32 tlas_slot,
                                    ResourceHandle depth, ResourceHandle normals,
                                    const Mat4& inv_view_proj, u32 frame_index, f32 near_plane,
                                    const f32 hit_dist_params[3]) {
  ResourceHandle hitdist =
      graph.CreateTexture({.name = "rtao_hitdist", .format = kHitDistFormat, .width = extent_.width,
                           .height = extent_.height});

  f32 hit_a = hit_dist_params[0], hit_b = hit_dist_params[1], hit_c = hit_dist_params[2];
  graph.AddPass(
      "rtao_trace",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(depth, ResourceUsage::kSampledCompute);
        builder.Read(normals, ResourceUsage::kSampledCompute);
        builder.Write(hitdist, ResourceUsage::kStorageWrite);
      },
      [this, &raytracing, tlas_slot, depth, normals, hitdist, inv_view_proj, frame_index, near_plane,
       hit_a, hit_b, hit_c](PassContext& ctx) {
        TracePush push{};
        push.inv_view_proj = inv_view_proj;
        push.inv_size[0] = 1.0f / static_cast<f32>(extent_.width);
        push.inv_size[1] = 1.0f / static_cast<f32>(extent_.height);
        push.radius = settings_.radius;
        push.near_plane = near_plane;
        push.hit_a = hit_a;
        push.hit_b = hit_b;
        push.hit_c = hit_c;
        push.frame_index = static_cast<f32>(frame_index % 4096);
        push.ray_count = settings_.ray_count;

        ctx.cmd->BindPipeline(pipeline_);
        ctx.cmd->BindTransient(0, {Bind::Storage(0, ctx.graph->image(hitdist)),
                                   Bind::Sampled(1, ctx.graph->image(depth)),
                                   Bind::Sampled(2, ctx.graph->image(normals)),
                                   Bind::Accel(3, raytracing.tlas(tlas_slot))});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D(extent_);
      });
  return hitdist;
}

}  // namespace rec::render
