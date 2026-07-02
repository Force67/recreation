#include "render/pipeline/gpu_cull.h"

#include <cmath>
#include <cstring>

#include "core/log.h"
#include "render/rhi/device.h"
#include "shaders/bounds_ps_hlsl.h"
#include "shaders/bounds_vs_hlsl.h"
#include "shaders/cull_cs_hlsl.h"
#include "shaders/hiz_reduce_cs_hlsl.h"

namespace rec::render {
namespace {

struct CullPush {
  f32 planes[5][4];   // 80
  Mat4 prev_view_proj;
  f32 eye_pad[4];     // xyz eye
  f32 proj_hiz[4];    // proj.m00, proj.m11, hiz w, hiz h
  u32 misc[4];        // instance_count, frustum_enabled, occlusion_enabled, pad
};

struct HizPush {
  u32 dst_size[2];
  u32 block;
};

// Gribb-Hartmann frustum planes from a column-major view_proj (clip = vp*world).
// Normalized, oriented so a point is inside when dot(n, p) + d >= 0. Returns the
// four side planes plus near; the reversed-z far plane is skipped (conservative).
void ExtractPlanes(const Mat4& vp, f32 out[5][4]) {
  const f32* m = vp.m;
  auto row = [&](int r, int c) { return m[c * 4 + r]; };
  // left, right, bottom, top, near
  f32 p[5][4] = {
      {row(3, 0) + row(0, 0), row(3, 1) + row(0, 1), row(3, 2) + row(0, 2), row(3, 3) + row(0, 3)},
      {row(3, 0) - row(0, 0), row(3, 1) - row(0, 1), row(3, 2) - row(0, 2), row(3, 3) - row(0, 3)},
      {row(3, 0) + row(1, 0), row(3, 1) + row(1, 1), row(3, 2) + row(1, 2), row(3, 3) + row(1, 3)},
      {row(3, 0) - row(1, 0), row(3, 1) - row(1, 1), row(3, 2) - row(1, 2), row(3, 3) - row(1, 3)},
      {row(2, 0), row(2, 1), row(2, 2), row(2, 3)},  // near: clip.z >= 0
  };
  for (int i = 0; i < 5; ++i) {
    f32 len = std::sqrt(p[i][0] * p[i][0] + p[i][1] * p[i][1] + p[i][2] * p[i][2]);
    if (len < 1e-8f) len = 1.0f;
    for (int c = 0; c < 4; ++c) out[i][c] = p[i][c] / len;
  }
}

}  // namespace

bool GpuCull::Initialize(Device& device, Format color_format) {
  pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_cull_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageBuffer},
                          {1, BindingType::kStorageBuffer},
                          {2, BindingType::kStorageBuffer},
                          {3, BindingType::kSampledImage}}}},  // hi-z (occlusion)
      .push_constant_size = sizeof(CullPush),
      .debug_name = "cull",
  });
  if (!pipeline_) {
    REC_ERROR("cull pipeline creation failed");
    return false;
  }

  for (u32 i = 0; i < kFramesInFlight; ++i) {
    instances_[i] = device.CreateBuffer(static_cast<u64>(kMaxInstances) * sizeof(Instance),
                                        kBufferUsageStorage, true);
    commands_[i] = device.CreateBuffer(static_cast<u64>(kMaxCommands) * sizeof(Command),
                                       kBufferUsageStorage | kBufferUsageIndirect, true);
    counts_[i] = device.CreateBuffer(16, kBufferUsageStorage, true);
    if (!instances_[i].mapped || !commands_[i].mapped || !counts_[i].mapped) return false;
  }

  // Hi-z reduce pipeline: storage dst + sampled src.
  hiz_pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_hiz_reduce_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kSampledImage}}}},
      .push_constant_size = sizeof(HizPush),
      .debug_name = "cull_hiz_reduce",
  });
  if (!hiz_pipeline_) return false;

  return CreateBoundsPipeline(device, color_format);
}

void GpuCull::ResizeDepth(Device& device, u32 width, u32 height) {
  if (width == depth_w_ && height == depth_h_) return;
  for (u32 i = 0; i < kFramesInFlight; ++i) {
    device.DestroyImage(prev_depth_[i]);
    prev_depth_[i] = device.CreateImage2D(Format::kR32Float, {width, height},
                                          kTextureUsageStorage | kTextureUsageSampled);
    prev_depth_state_[i] = ResourceState::kUndefined;
  }
  depth_w_ = width;
  depth_h_ = height;
  hiz_w_ = (width + kHizDownsample - 1) / kHizDownsample;
  hiz_h_ = (height + kHizDownsample - 1) / kHizDownsample;
}

ResourceHandle GpuCull::BuildHiZ(RenderGraph& graph, u32 slot) {
  if (depth_w_ == 0) return kInvalidResource;
  u32 read = (slot + 1) % kFramesInFlight;  // last frame's snapshot
  ResourceHandle prev = graph.ImportImage("cull_prev_depth", prev_depth_[read],
                                          &prev_depth_state_[read]);
  ResourceHandle hiz =
      graph.CreateTexture({.name = "cull_hiz", .format = Format::kR32Float,
                           .width = hiz_w_, .height = hiz_h_});
  graph.AddPass(
      "cull_hiz",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(prev, ResourceUsage::kSampledCompute);
        builder.Write(hiz, ResourceUsage::kStorageWrite);
      },
      [this, prev, hiz](PassContext& ctx) {
        ctx.cmd->BindPipeline(hiz_pipeline_);
        ctx.cmd->BindTransient(0, {Bind::Storage(0, ctx.graph->image(hiz)),
                                   Bind::Sampled(1, ctx.graph->image(prev))});
        HizPush push{{hiz_w_, hiz_h_}, kHizDownsample};
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D({hiz_w_, hiz_h_});
      });
  return hiz;
}

void GpuCull::CopyDepth(RenderGraph& graph, ResourceHandle depth_export, u32 slot) {
  if (depth_w_ == 0) return;
  ResourceHandle dst =
      graph.ImportImage("cull_depth_snapshot", prev_depth_[slot], &prev_depth_state_[slot]);
  graph.AddPass(
      "cull_depth_copy",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(depth_export, ResourceUsage::kSampledCompute);
        builder.Write(dst, ResourceUsage::kStorageWrite);
      },
      [this, depth_export, dst](PassContext& ctx) {
        ctx.cmd->BindPipeline(hiz_pipeline_);
        ctx.cmd->BindTransient(0, {Bind::Storage(0, ctx.graph->image(dst)),
                                   Bind::Sampled(1, ctx.graph->image(depth_export))});
        HizPush push{{depth_w_, depth_h_}, 1};  // 1:1 snapshot
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D({depth_w_, depth_h_});
      });
}

bool GpuCull::CreateBoundsPipeline(Device& device, Format color_format) {
  bounds_pipeline_ = device.CreateGraphicsPipeline({
      .vertex = REC_SHADER(k_bounds_vs_hlsl),
      .fragment = REC_SHADER(k_bounds_ps_hlsl),
      .topology = PrimitiveTopology::kLineList,
      .raster = {.cull = CullMode::kNone},  // overlay, no depth
      .color_formats = {color_format},
      .sets = {{.slots = {{0, BindingType::kStorageBuffer}}, .stages = kShaderStageVertex}},
      .push_constant_size = sizeof(Mat4),
      .debug_name = "bounds_debug",
  });
  if (!bounds_pipeline_) {
    REC_ERROR("bounds pipeline creation failed");
    return false;
  }
  return true;
}

void GpuCull::AddBoundsPass(RenderGraph& graph, ResourceHandle color, const Mat4& view_proj,
                            u32 instance_count, u32 slot) {
  if (instance_count == 0) return;
  GpuBuffer instances = instances_[slot];
  graph.AddPass(
      "bounds_debug",
      [&](RenderGraph::PassBuilder& builder) { builder.Write(color, ResourceUsage::kColorAttachment); },
      [this, color, instances, view_proj, instance_count](PassContext& ctx) {
        const GpuImage& target = ctx.graph->image(color);
        ColorAttachment attachment{.view = target.view, .load = LoadOp::kLoad};
        ctx.cmd->BeginRendering({.extent = target.extent, .colors = {&attachment, 1}});
        ctx.cmd->BindPipeline(bounds_pipeline_);
        ctx.cmd->BindTransient(0, {Bind::StorageBuffer(0, instances)});
        ctx.cmd->Push(view_proj);
        ctx.cmd->Draw(24, instance_count, 0, 0);
        ctx.cmd->EndRendering();
      });
}

GpuCull::Instance* GpuCull::instances(u32 slot) {
  return static_cast<Instance*>(instances_[slot].mapped);
}

GpuCull::Command* GpuCull::commands(u32 slot) {
  return static_cast<Command*>(commands_[slot].mapped);
}

u32 GpuCull::last_visible(u32 slot) const {
  return *static_cast<const u32*>(counts_[slot].mapped);
}

void GpuCull::AddToGraph(RenderGraph& graph, const Mat4& view_proj, const Mat4& prev_view_proj,
                         const f32 proj_scale[2], const Vec3& eye, u32 instance_count, bool frustum,
                         bool occlusion, ResourceHandle hiz, u32 slot) {
  if (instance_count == 0) return;
  *static_cast<u32*>(counts_[slot].mapped) = 0;  // reset the visible counter

  bool occ = occlusion && hiz != kInvalidResource;
  CullPush push{};
  ExtractPlanes(view_proj, push.planes);
  push.prev_view_proj = prev_view_proj;
  push.eye_pad[0] = eye.x;
  push.eye_pad[1] = eye.y;
  push.eye_pad[2] = eye.z;
  push.proj_hiz[0] = proj_scale[0];
  push.proj_hiz[1] = proj_scale[1];
  push.proj_hiz[2] = static_cast<f32>(hiz_w_);
  push.proj_hiz[3] = static_cast<f32>(hiz_h_);
  push.misc[0] = instance_count;
  push.misc[1] = frustum ? 1u : 0u;
  push.misc[2] = occ ? 1u : 0u;
  GpuBuffer instances = instances_[slot];
  GpuBuffer commands = commands_[slot];
  GpuBuffer counts = counts_[slot];

  bool has_hiz = hiz != kInvalidResource;
  graph.AddPass(
      "cull",
      [&](RenderGraph::PassBuilder& builder) {
        if (has_hiz) builder.Read(hiz, ResourceUsage::kSampledCompute);
      },
      [this, push, instances, commands, counts, instance_count, has_hiz, hiz](PassContext& ctx) {
        ctx.cmd->BindPipeline(pipeline_);
        base::Vector<BindingItem> items;
        items.push_back(Bind::StorageBuffer(0, instances));
        items.push_back(Bind::StorageBuffer(1, commands));
        items.push_back(Bind::StorageBuffer(2, counts));
        if (has_hiz) items.push_back(Bind::Sampled(3, ctx.graph->image(hiz)));
        ctx.cmd->BindTransient(0, {items.data(), items.size()});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch((instance_count + 63) / 64, 1, 1);

        // Make the written instanceCounts visible to the indirect draws.
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kIndirectArgs);
      });
}

void GpuCull::Destroy(Device& device) {
  if (pipeline_) device.DestroyPipeline(pipeline_);
  if (bounds_pipeline_) device.DestroyPipeline(bounds_pipeline_);
  if (hiz_pipeline_) device.DestroyPipeline(hiz_pipeline_);
  for (u32 i = 0; i < kFramesInFlight; ++i) {
    device.DestroyBuffer(instances_[i]);
    device.DestroyBuffer(commands_[i]);
    device.DestroyBuffer(counts_[i]);
    device.DestroyImage(prev_depth_[i]);
  }
}

}  // namespace rec::render
