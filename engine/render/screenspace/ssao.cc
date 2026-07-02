#include "render/screenspace/ssao.h"

#include "core/log.h"
#include "render/rhi/device.h"
#include "shaders/ssao_cs_hlsl.h"

namespace rec::render {
namespace {

struct SsaoPush {
  Mat4 inv_view_proj;
  f32 inv_size[2];
  f32 proj_scale[2];
  f32 radius;
  f32 near_plane;
  f32 intensity;
  f32 bias;
  f32 power;
  f32 frame_index;
  u32 sample_count;
  u32 pad;
};

}  // namespace

bool SsaoPass::Initialize(Device& device) {
  pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_ssao_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kSampledImage},
                          {2, BindingType::kSampledImage}}}},
      .push_constant_size = sizeof(SsaoPush),
      .debug_name = "ssao",
  });
  return static_cast<bool>(pipeline_);
}

void SsaoPass::Destroy(Device& device) {
  device.DestroyPipeline(pipeline_);
  pipeline_ = {};
}

ResourceHandle SsaoPass::AddToGraph(RenderGraph& graph, ResourceHandle depth,
                                    ResourceHandle normals, const Mat4& inv_view_proj,
                                    const f32 proj_scale[2], f32 near_plane, u32 frame_index) {
  ResourceHandle ao = graph.CreateTexture(
      {.name = "ssao", .format = kAoFormat, .width = extent_.width, .height = extent_.height});

  f32 scale_x = proj_scale[0], scale_y = proj_scale[1];
  graph.AddPass(
      "ssao",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(depth, ResourceUsage::kSampledCompute);
        builder.Read(normals, ResourceUsage::kSampledCompute);
        builder.Write(ao, ResourceUsage::kStorageWrite);
      },
      [this, depth, normals, ao, inv_view_proj, scale_x, scale_y, near_plane,
       frame_index](PassContext& ctx) {
        SsaoPush push{};
        push.inv_view_proj = inv_view_proj;
        push.inv_size[0] = 1.0f / static_cast<f32>(extent_.width);
        push.inv_size[1] = 1.0f / static_cast<f32>(extent_.height);
        push.proj_scale[0] = scale_x;
        push.proj_scale[1] = scale_y;
        push.radius = settings_.radius;
        push.near_plane = near_plane;
        push.intensity = settings_.intensity;
        push.bias = 0.02f;
        push.power = settings_.power;
        push.frame_index = static_cast<f32>(frame_index % 4096);
        push.sample_count = settings_.sample_count;

        ctx.cmd->BindPipeline(pipeline_);
        ctx.cmd->BindTransient(0, {Bind::Storage(0, ctx.graph->image(ao)),
                                   Bind::Sampled(1, ctx.graph->image(depth)),
                                   Bind::Sampled(2, ctx.graph->image(normals))});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D(extent_);
      });
  return ao;
}

}  // namespace rec::render
