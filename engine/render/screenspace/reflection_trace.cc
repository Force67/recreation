#include "render/screenspace/reflection_trace.h"

#include "core/log.h"
#include "render/gi/raytracing.h"
#include "render/rhi/device.h"
#include "shaders/reflection_trace_cs_hlsl.h"

namespace rec::render {
namespace {

struct ReflectionPush {
  Mat4 inv_view_proj;
  f32 camera_pos[4];
  f32 sun_direction[4];
  f32 sun_color[4];
  f32 inv_size[2];
  f32 near_plane;
  f32 roughness_cutoff;
  f32 hit_a;
  f32 hit_b;
  f32 hit_c;
  f32 frame_index;
  u32 flags;
  f32 pad[3];
};

}  // namespace

bool ReflectionTrace::Initialize(Device& device, BindingLayoutHandle bindless_layout) {
  pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_reflection_trace_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kSampledImage},
                          {2, BindingType::kSampledImage},
                          {3, BindingType::kAccelStruct},
                          {4, BindingType::kCombinedTextureSampler},
                          {5, BindingType::kCombinedTextureSampler},
                          {6, BindingType::kUniformBuffer}}},
               {.shared = bindless_layout}},
      .push_constant_size = sizeof(ReflectionPush),
      .debug_name = "reflection_trace",
  });
  if (!pipeline_) {
    REC_ERROR("reflection trace pipeline creation failed");
    return false;
  }
  return true;
}

void ReflectionTrace::Destroy(Device& device) {
  if (pipeline_) {
    device.DestroyPipeline(pipeline_);
    pipeline_ = {};
  }
}

ResourceHandle ReflectionTrace::AddToGraph(RenderGraph& graph, RayTracingContext& raytracing,
                                           u32 tlas_slot, BindingSetHandle bindless_set,
                                           ResourceHandle depth, ResourceHandle normals,
                                           TextureView prefiltered, TextureView ddgi_irradiance,
                                           bool ddgi_in_general, const GpuBuffer& ddgi_volume,
                                           u64 ddgi_volume_size, SamplerHandle sampler,
                                           Extent2D extent, const Frame& frame) {
  ResourceHandle out = graph.CreateTexture({.name = "spec_reflection_raw",
                                            .format = Format::kRGBA16Float,
                                            .width = extent.width,
                                            .height = extent.height});
  graph.AddPass(
      "reflection_trace",
      [&](RenderGraph::PassBuilder& b) {
        b.Write(out, ResourceUsage::kStorageWrite);
        b.Read(depth, ResourceUsage::kSampledCompute);
        b.Read(normals, ResourceUsage::kSampledCompute);
      },
      [this, &raytracing, tlas_slot, bindless_set, out, depth, normals, prefiltered,
       ddgi_irradiance, ddgi_in_general, ddgi_volume, ddgi_volume_size, sampler, extent,
       frame](PassContext& ctx) {
        BindingItem ddgi_item = Bind::Combined(5, ddgi_irradiance, sampler);
        if (ddgi_in_general) ddgi_item = InGeneral(ddgi_item);
        base::Vector<BindingItem> items;
        items.push_back(Bind::Storage(0, ctx.graph->image(out)));
        items.push_back(Bind::Sampled(1, ctx.graph->image(depth)));
        items.push_back(Bind::Sampled(2, ctx.graph->image(normals)));
        items.push_back(Bind::Accel(3, raytracing.tlas(tlas_slot)));
        items.push_back(Bind::Combined(4, prefiltered, sampler));
        items.push_back(ddgi_item);
        items.push_back(Bind::Uniform(6, ddgi_volume, 0, ddgi_volume_size));

        ReflectionPush p{};
        p.inv_view_proj = frame.inv_view_proj;
        p.camera_pos[0] = frame.camera_pos.x;
        p.camera_pos[1] = frame.camera_pos.y;
        p.camera_pos[2] = frame.camera_pos.z;
        Vec3 sun = Normalize(frame.sun_direction);
        p.sun_direction[0] = sun.x;
        p.sun_direction[1] = sun.y;
        p.sun_direction[2] = sun.z;
        p.sun_direction[3] = frame.sun_intensity;
        p.sun_color[0] = frame.sun_color.x;
        p.sun_color[1] = frame.sun_color.y;
        p.sun_color[2] = frame.sun_color.z;
        p.inv_size[0] = 1.0f / static_cast<f32>(extent.width);
        p.inv_size[1] = 1.0f / static_cast<f32>(extent.height);
        p.near_plane = frame.near_plane;
        p.roughness_cutoff = frame.roughness_cutoff;
        p.hit_a = frame.hit_dist_params[0];
        p.hit_b = frame.hit_dist_params[1];
        p.hit_c = frame.hit_dist_params[2];
        p.frame_index = static_cast<f32>(frame.frame_index % 64u);
        p.flags = frame.ddgi ? 1u : 0u;
        ctx.cmd->BindPipeline(pipeline_);
        ctx.cmd->BindTransient(0, {items.data(), items.size()});
        ctx.cmd->BindSet(1, bindless_set);
        ctx.cmd->Push(p);
        ctx.cmd->Dispatch2D(extent);
      });
  return out;
}

}  // namespace rec::render
