#include "render/screenspace/ssr.h"

#include "core/log.h"
#include "render/rhi/device.h"
#include "shaders/ssr_cs_hlsl.h"

namespace rec::render {
namespace {

struct SsrPush {
  Mat4 view_proj;
  Mat4 inv_view_proj;
  f32 camera_pos[4];
  f32 inv_size[2];
  f32 intensity;
  f32 max_distance;
  f32 thickness;
  f32 frame_index;
  u32 step_count;
  u32 pad;
};

}  // namespace

bool SsrPass::Initialize(Device& device) {
  // 0: output color (storage), 1: depth, 2: normals, 3: scene color (all sampled).
  pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_ssr_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kSampledImage},
                          {2, BindingType::kSampledImage},
                          {3, BindingType::kSampledImage}}}},
      .push_constant_size = sizeof(SsrPush),
      .debug_name = "ssr",
  });
  if (!pipeline_) {
    REC_ERROR("ssr pipeline creation failed");
    return false;
  }
  return true;
}

void SsrPass::Destroy(Device& device) {
  device.DestroyPipeline(pipeline_);
  pipeline_ = {};
}

ResourceHandle SsrPass::AddToGraph(RenderGraph& graph, ResourceHandle scene_color,
                                   ResourceHandle depth, ResourceHandle normals,
                                   const Mat4& view_proj, const Mat4& inv_view_proj,
                                   const Vec3& camera_pos, u32 frame_index) {
  ResourceHandle out = graph.CreateTexture({.name = "ssr",
                                            .format = Format::kRGBA16Float,
                                            .width = extent_.width,
                                            .height = extent_.height});

  graph.AddPass(
      "ssr",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(scene_color, ResourceUsage::kSampledCompute);
        builder.Read(depth, ResourceUsage::kSampledCompute);
        builder.Read(normals, ResourceUsage::kSampledCompute);
        builder.Write(out, ResourceUsage::kStorageWrite);
      },
      [this, scene_color, depth, normals, out, view_proj, inv_view_proj, camera_pos,
       frame_index](PassContext& ctx) {
        SsrPush push{};
        push.view_proj = view_proj;
        push.inv_view_proj = inv_view_proj;
        push.camera_pos[0] = camera_pos.x;
        push.camera_pos[1] = camera_pos.y;
        push.camera_pos[2] = camera_pos.z;
        push.inv_size[0] = 1.0f / static_cast<f32>(extent_.width);
        push.inv_size[1] = 1.0f / static_cast<f32>(extent_.height);
        push.intensity = settings_.intensity;
        push.max_distance = settings_.max_distance;
        push.thickness = settings_.thickness;
        push.frame_index = static_cast<f32>(frame_index % 4096);
        push.step_count = settings_.step_count;

        ctx.cmd->BindPipeline(pipeline_);
        ctx.cmd->BindTransient(0, {Bind::Storage(0, ctx.graph->image(out)),
                                   Bind::Sampled(1, ctx.graph->image(depth)),
                                   Bind::Sampled(2, ctx.graph->image(normals)),
                                   Bind::Sampled(3, ctx.graph->image(scene_color))});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D(extent_);
      });
  return out;
}

}  // namespace rec::render
