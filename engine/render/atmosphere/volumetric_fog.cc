#include "render/atmosphere/volumetric_fog.h"

#include "core/log.h"
#include "render/gi/raytracing.h"
#include "render/rhi/device.h"
#include "shaders/fog_cs_hlsl.h"

namespace rec::render {
namespace {

struct FogPush {
  Mat4 inv_view_proj;
  f32 camera_pos[4];
  f32 sun_direction[4];
  f32 sun_color[4];
  f32 params[4];  // density, height falloff, base height, max distance
  u32 size[2];
  u32 steps;
  u32 frame_index;
};

}  // namespace

bool VolumetricFog::Initialize(Device& device) {
  pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_fog_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kSampledImage},
                          {2, BindingType::kSampledImage},
                          {3, BindingType::kAccelStruct}}}},
      .push_constant_size = sizeof(FogPush),
      .debug_name = "volumetric_fog",
  });
  if (!pipeline_) {
    REC_ERROR("volumetric fog pipeline creation failed");
    return false;
  }
  return true;
}

void VolumetricFog::Destroy(Device& device) {
  device.DestroyPipeline(pipeline_);
  pipeline_ = {};
}

ResourceHandle VolumetricFog::AddToGraph(RenderGraph& graph, RayTracingContext& raytracing,
                                         u32 tlas_slot, ResourceHandle color, ResourceHandle depth,
                                         Extent2D extent, const Frame& frame) {
  ResourceHandle fogged = graph.CreateTexture({.name = "fogged",
                                               .format = Format::kRGBA16Float,
                                               .width = extent.width, .height = extent.height});
  graph.AddPass(
      "volumetric_fog",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(color, ResourceUsage::kSampledCompute);
        builder.Read(depth, ResourceUsage::kSampledCompute);
        builder.Write(fogged, ResourceUsage::kStorageWrite);
      },
      [this, &raytracing, tlas_slot, color, depth, fogged, extent, frame](PassContext& ctx) {
        FogPush push{};
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
        push.sun_color[3] = frame.anisotropy;
        push.params[0] = frame.density;
        push.params[1] = frame.height_falloff;
        push.params[2] = frame.base_height;
        push.params[3] = frame.max_distance;
        push.size[0] = extent.width;
        push.size[1] = extent.height;
        push.steps = frame.steps;
        push.frame_index = frame.frame_index;

        ctx.cmd->BindPipeline(pipeline_);
        ctx.cmd->BindTransient(0, {Bind::Storage(0, ctx.graph->image(fogged)),
                                   Bind::Sampled(1, ctx.graph->image(color)),
                                   Bind::Sampled(2, ctx.graph->image(depth)),
                                   Bind::Accel(3, raytracing.tlas(tlas_slot))});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D(extent);
      });
  return fogged;
}

}  // namespace rec::render
