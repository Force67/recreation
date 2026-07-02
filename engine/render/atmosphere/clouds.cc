#include "render/atmosphere/clouds.h"

#include "core/log.h"
#include "render/rhi/device.h"
#include "shaders/clouds_cs_hlsl.h"

namespace rec::render {
namespace {

struct CloudPush {
  Mat4 inv_view_proj;
  f32 camera_pos[4];     // xyz eye, w time
  f32 sun_direction[4];  // xyz travel dir, w intensity
  f32 sun_color[4];      // rgb, w coverage
  f32 params[4];         // bottom, top, density, wind
  u32 size[2];
  u32 steps;
  u32 light_steps;
};

}  // namespace

bool Clouds::Initialize(Device& device) {
  pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_clouds_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kSampledImage},
                          {2, BindingType::kSampledImage}}}},
      .push_constant_size = sizeof(CloudPush),
      .debug_name = "clouds",
  });
  if (!pipeline_) {
    REC_ERROR("clouds pipeline creation failed");
    return false;
  }
  return true;
}

void Clouds::Destroy(Device& device) {
  device.DestroyPipeline(pipeline_);
  pipeline_ = {};
}

ResourceHandle Clouds::AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                                  Extent2D extent, const Frame& frame) {
  ResourceHandle out = graph.CreateTexture({.name = "clouds",
                                            .format = Format::kRGBA16Float,
                                            .width = extent.width,
                                            .height = extent.height});
  graph.AddPass(
      "clouds",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(color, ResourceUsage::kSampledCompute);
        builder.Read(depth, ResourceUsage::kSampledCompute);
        builder.Write(out, ResourceUsage::kStorageWrite);
      },
      [this, color, depth, out, extent, frame](PassContext& ctx) {
        CloudPush push{};
        push.inv_view_proj = frame.inv_view_proj;
        push.camera_pos[0] = frame.camera_pos.x;
        push.camera_pos[1] = frame.camera_pos.y;
        push.camera_pos[2] = frame.camera_pos.z;
        push.camera_pos[3] = frame.time;
        Vec3 sun = Normalize(frame.sun_direction);
        push.sun_direction[0] = sun.x;
        push.sun_direction[1] = sun.y;
        push.sun_direction[2] = sun.z;
        push.sun_direction[3] = frame.sun_intensity;
        push.sun_color[0] = frame.sun_color.x;
        push.sun_color[1] = frame.sun_color.y;
        push.sun_color[2] = frame.sun_color.z;
        push.sun_color[3] = frame.coverage;
        push.params[0] = frame.bottom;
        push.params[1] = frame.top;
        push.params[2] = frame.density;
        push.params[3] = frame.wind;
        push.size[0] = extent.width;
        push.size[1] = extent.height;
        push.steps = frame.steps;
        push.light_steps = frame.light_steps;

        ctx.cmd->BindPipeline(pipeline_);
        ctx.cmd->BindTransient(0, {Bind::Storage(0, ctx.graph->image(out)),
                                   Bind::Sampled(1, ctx.graph->image(color)),
                                   Bind::Sampled(2, ctx.graph->image(depth))});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D(extent);
      });
  return out;
}

}  // namespace rec::render
