#include "render/gi/restir_di.h"

#include "core/log.h"
#include "shaders/restir_di_spatial_cs_hlsl.h"
#include "shaders/restir_di_temporal_cs_hlsl.h"

namespace rec::render {
namespace {

struct TemporalPush {
  Mat4 inv_view_proj;
  u32 size[2];
  u32 frame_index;
  u32 light_count;
  u32 candidates;
  f32 m_max;
  f32 reset;
  f32 pad0;
};

struct SpatialPush {
  Mat4 inv_view_proj;
  f32 camera_pos[4];
  u32 size[2];
  u32 frame_index;
  u32 light_count;
  u32 sample_count;
  f32 radius;
  f32 m_max;
  f32 pad0;
};

}  // namespace

bool RestirDi::Initialize(Device& device) {
  temporal_pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_restir_di_temporal_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kSampledImage},
                          {2, BindingType::kSampledImage},
                          {3, BindingType::kSampledImage},
                          {4, BindingType::kSampledImage},
                          {5, BindingType::kSampledImage},
                          {6, BindingType::kSampledImage},
                          {7, BindingType::kStorageBuffer}}}},
      .push_constant_size = sizeof(TemporalPush),
      .debug_name = "restir_di_temporal",
  });
  spatial_pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_restir_di_spatial_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kStorageImage},
                          {2, BindingType::kSampledImage},
                          {3, BindingType::kSampledImage},
                          {4, BindingType::kSampledImage},
                          {5, BindingType::kStorageBuffer},
                          {6, BindingType::kAccelStruct},
                          {7, BindingType::kStorageImage},
                          {8, BindingType::kStorageImage},
                          {9, BindingType::kStorageImage}}}},
      .push_constant_size = sizeof(SpatialPush),
      .debug_name = "restir_di_spatial",
  });
  if (!temporal_pipeline_ || !spatial_pipeline_) {
    REC_ERROR("restir di pipeline creation failed");
    return false;
  }
  return true;
}

void RestirDi::Destroy(Device& device) {
  for (PipelineHandle* p : {&temporal_pipeline_, &spatial_pipeline_}) {
    if (*p) device.DestroyPipeline(*p);
    *p = {};
  }
  for (GpuImage& image : reservoir_) {
    if (image) device.DestroyImage(image);
    image = {};
  }
  if (prev_depth_) device.DestroyImage(prev_depth_);
  prev_depth_ = {};
  if (prev_normal_) device.DestroyImage(prev_normal_);
  prev_normal_ = {};
}

bool RestirDi::Resize(Device& device, Extent2D extent) {
  if (!temporal_pipeline_) return false;
  if (reservoir_[0] && extent.width == extent_.width && extent.height == extent_.height) {
    return true;
  }
  for (GpuImage& image : reservoir_) {
    if (image) device.DestroyImage(image);
  }
  if (prev_depth_) device.DestroyImage(prev_depth_);
  if (prev_normal_) device.DestroyImage(prev_normal_);
  extent_ = extent;

  const TextureUsageFlags usage = kTextureUsageStorage | kTextureUsageSampled;
  reservoir_[0] = device.CreateImage2D(Format::kRGBA32Float, extent, usage);
  reservoir_[1] = device.CreateImage2D(Format::kRGBA32Float, extent, usage);
  prev_depth_ = device.CreateImage2D(Format::kR32Float, extent, usage);
  prev_normal_ = device.CreateImage2D(Format::kRGBA16Float, extent, usage);
  if (!reservoir_[0] || !reservoir_[1] || !prev_depth_ || !prev_normal_) {
    REC_WARN("restir di history allocation failed");
    return false;
  }
  device.ImmediateSubmit([this](CommandList& cmd) {
    TextureBarrier to_general[4] = {
        Transition(reservoir_[0], ResourceState::kUndefined, ResourceState::kGeneral),
        Transition(reservoir_[1], ResourceState::kUndefined, ResourceState::kGeneral),
        Transition(prev_depth_, ResourceState::kUndefined, ResourceState::kGeneral),
        Transition(prev_normal_, ResourceState::kUndefined, ResourceState::kGeneral)};
    cmd.TextureBarriers(to_general);
  });
  reset_ = true;
  return true;
}

RestirDi::Outputs RestirDi::AddToGraph(RenderGraph& graph, ResourceHandle depth_export,
                                       ResourceHandle normals, ResourceHandle motion,
                                       RayTracingContext& raytracing, Extent2D extent,
                                       const Frame& frame) {
  Outputs out;
  if (!available() || !frame.lights || frame.light_count == 0) return out;

  out.diffuse = graph.CreateTexture({.name = "restir_di_diffuse",
                                     .format = Format::kRGBA16Float,
                                     .width = extent.width,
                                     .height = extent.height});
  out.spec = graph.CreateTexture({.name = "restir_di_spec",
                                  .format = Format::kRGBA16Float,
                                  .width = extent.width,
                                  .height = extent.height});

  const bool reset = reset_;
  reset_ = false;

  graph.AddPass(
      "restir_di_temporal",
      [&](RenderGraph::PassBuilder& b) {
        b.Read(depth_export, ResourceUsage::kSampledCompute);
        b.Read(normals, ResourceUsage::kSampledCompute);
        b.Read(motion, ResourceUsage::kSampledCompute);
      },
      [this, depth_export, normals, motion, extent, frame, reset](PassContext& ctx) {
        TemporalPush push{};
        push.inv_view_proj = frame.inv_view_proj;
        push.size[0] = extent.width;
        push.size[1] = extent.height;
        push.frame_index = frame.frame_index;
        push.light_count = frame.light_count;
        push.candidates = 8;
        push.m_max = 20.0f;
        push.reset = reset ? 1.0f : 0.0f;

        ctx.cmd->BindPipeline(temporal_pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::Storage(0, reservoir_[0]),
                Bind::Sampled(1, ctx.graph->image(depth_export)),
                Bind::Sampled(2, ctx.graph->image(normals)),
                Bind::Sampled(3, ctx.graph->image(motion)),
                InGeneral(Bind::Sampled(4, prev_depth_)),
                InGeneral(Bind::Sampled(5, prev_normal_)),
                InGeneral(Bind::Sampled(6, reservoir_[1])),
                Bind::StorageBuffer(7, frame.lights, 0, frame.lights.size)});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D(extent);
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);
      });

  graph.AddPass(
      "restir_di_spatial",
      [&](RenderGraph::PassBuilder& b) {
        b.Write(out.diffuse, ResourceUsage::kStorageWrite);
        b.Write(out.spec, ResourceUsage::kStorageWrite);
        b.Read(depth_export, ResourceUsage::kSampledCompute);
        b.Read(normals, ResourceUsage::kSampledCompute);
      },
      [this, out, depth_export, normals, &raytracing, extent, frame](PassContext& ctx) {
        SpatialPush push{};
        push.inv_view_proj = frame.inv_view_proj;
        push.camera_pos[0] = frame.camera_pos.x;
        push.camera_pos[1] = frame.camera_pos.y;
        push.camera_pos[2] = frame.camera_pos.z;
        push.camera_pos[3] = 1.0f;
        push.size[0] = extent.width;
        push.size[1] = extent.height;
        push.frame_index = frame.frame_index;
        push.light_count = frame.light_count;
        push.sample_count = 4;
        push.radius = 16.0f;
        push.m_max = 60.0f;

        ctx.cmd->BindPipeline(spatial_pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::Storage(0, ctx.graph->image(out.diffuse)),
                Bind::Storage(1, ctx.graph->image(out.spec)),
                InGeneral(Bind::Sampled(2, reservoir_[0])),
                Bind::Sampled(3, ctx.graph->image(depth_export)),
                Bind::Sampled(4, ctx.graph->image(normals)),
                Bind::StorageBuffer(5, frame.lights, 0, frame.lights.size),
                Bind::Accel(6, raytracing.tlas(frame.tlas_slot)),
                Bind::Storage(7, reservoir_[1]),
                Bind::Storage(8, prev_depth_),
                Bind::Storage(9, prev_normal_)});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D(extent);
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);
      });

  return out;
}

}  // namespace rec::render
