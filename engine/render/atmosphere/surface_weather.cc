#include "render/atmosphere/surface_weather.h"

#include "core/log.h"
#include "render/rhi/device.h"
#include "shaders/surface_weather_cs_hlsl.h"

namespace rec::render {
namespace {

struct SurfacePush {
  Mat4 inv_view_proj;
  f32 camera_pos[4];  // xyz eye
  f32 params[4];      // wetness, snow, unused, unused
  u32 size[2];
  u32 pad[2];
};

}  // namespace

bool SurfaceWeather::Initialize(Device& device) {
  pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_surface_weather_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kSampledImage},
                          {2, BindingType::kSampledImage},
                          {3, BindingType::kSampledImage},
                          {4, BindingType::kCombinedTextureSampler}}}},
      .push_constant_size = sizeof(SurfacePush),
      .debug_name = "surface_weather",
  });
  if (!pipeline_) {
    REC_ERROR("surface weather pipeline creation failed");
    return false;
  }
  return true;
}

void SurfaceWeather::Destroy(Device& device) {
  device.DestroyPipeline(pipeline_);
  pipeline_ = {};
}

ResourceHandle SurfaceWeather::AddToGraph(RenderGraph& graph, ResourceHandle color,
                                          ResourceHandle normals, ResourceHandle depth,
                                          TextureView sky_view, SamplerHandle sky_sampler,
                                          Extent2D extent, const Frame& frame) {
  ResourceHandle out = graph.CreateTexture({.name = "surface_weather",
                                            .format = Format::kRGBA16Float,
                                            .width = extent.width,
                                            .height = extent.height});
  graph.AddPass(
      "surface_weather",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(color, ResourceUsage::kSampledCompute);
        builder.Read(normals, ResourceUsage::kSampledCompute);
        builder.Read(depth, ResourceUsage::kSampledCompute);
        builder.Write(out, ResourceUsage::kStorageWrite);
      },
      [this, color, normals, depth, out, sky_view, sky_sampler, extent, frame](PassContext& ctx) {
        SurfacePush push{};
        push.inv_view_proj = frame.inv_view_proj;
        push.camera_pos[0] = frame.camera_pos.x;
        push.camera_pos[1] = frame.camera_pos.y;
        push.camera_pos[2] = frame.camera_pos.z;
        push.params[0] = frame.wetness;
        push.params[1] = frame.snow ? 1.0f : 0.0f;
        push.params[2] = frame.time;
        push.size[0] = extent.width;
        push.size[1] = extent.height;

        ctx.cmd->BindPipeline(pipeline_);
        ctx.cmd->BindTransient(0, {Bind::Storage(0, ctx.graph->image(out)),
                                   Bind::Sampled(1, ctx.graph->image(color)),
                                   Bind::Sampled(2, ctx.graph->image(normals)),
                                   Bind::Sampled(3, ctx.graph->image(depth)),
                                   Bind::Combined(4, sky_view, sky_sampler)});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D(extent);
      });
  return out;
}

}  // namespace rec::render
