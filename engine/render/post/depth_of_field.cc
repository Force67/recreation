#include "render/post/depth_of_field.h"

#include <cstring>

#include "core/log.h"
#include "shaders/dof_coc_cs_hlsl.h"
#include "shaders/dof_composite_cs_hlsl.h"
#include "shaders/dof_gather_cs_hlsl.h"

namespace rec::render {
namespace {

struct CocPush {
  u32 size[2];
  f32 near_plane;
  f32 aperture;
  f32 max_coc;
  f32 focus_speed;
  f32 focus_override;
  f32 pad0;
};
struct GatherPush {
  u32 size[2];
  f32 inv_size[2];
  f32 max_coc;
  f32 pad[3];
};
struct CompositePush {
  u32 size[2];
  f32 inv_size[2];
};

}  // namespace

bool DepthOfFieldPass::Initialize(Device& device) {
  coc_pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_dof_coc_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kSampledImage},
                          {2, BindingType::kStorageBuffer}}}},
      .push_constant_size = sizeof(CocPush),
      .debug_name = "dof_coc",
  });
  gather_pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_dof_gather_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kCombinedTextureSampler},
                          {2, BindingType::kCombinedTextureSampler}}}},
      .push_constant_size = sizeof(GatherPush),
      .debug_name = "dof_gather",
  });
  composite_pipeline_ = device.CreateComputePipeline({
      .shader = REC_SHADER(k_dof_composite_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kSampledImage},
                          {2, BindingType::kCombinedTextureSampler},
                          {3, BindingType::kSampledImage}}}},
      .push_constant_size = sizeof(CompositePush),
      .debug_name = "dof_composite",
  });
  if (!coc_pipeline_ || !gather_pipeline_ || !composite_pipeline_) {
    REC_ERROR("dof pipeline creation failed");
    return false;
  }
  focus_state_ = device.CreateBuffer(sizeof(f32), kBufferUsageStorage, true);
  if (!focus_state_.mapped) return false;
  std::memset(focus_state_.mapped, 0, sizeof(f32));
  sampler_ = device.GetSampler({.address_u = AddressMode::kClampToEdge,
                                .address_v = AddressMode::kClampToEdge});
  return true;
}

void DepthOfFieldPass::Destroy(Device& device) {
  for (PipelineHandle* p : {&coc_pipeline_, &gather_pipeline_, &composite_pipeline_}) {
    if (*p) device.DestroyPipeline(*p);
    *p = {};
  }
  if (focus_state_) device.DestroyBuffer(focus_state_);
}

ResourceHandle DepthOfFieldPass::AddToGraph(RenderGraph& graph, ResourceHandle color,
                                            ResourceHandle depth, Extent2D extent,
                                            const Frame& frame) {
  Extent2D half{(extent.width + 1) / 2, (extent.height + 1) / 2};
  ResourceHandle coc = graph.CreateTexture(
      {.name = "dof_coc", .format = Format::kR16Float, .width = extent.width,
       .height = extent.height});
  ResourceHandle gathered = graph.CreateTexture(
      {.name = "dof_gather", .format = Format::kRGBA16Float, .width = half.width,
       .height = half.height});
  ResourceHandle out = graph.CreateTexture(
      {.name = "dof_out", .format = Format::kRGBA16Float, .width = extent.width,
       .height = extent.height});

  graph.AddPass(
      "dof_coc",
      [&](RenderGraph::PassBuilder& b) {
        b.Write(coc, ResourceUsage::kStorageWrite);
        b.Read(depth, ResourceUsage::kSampledCompute);
      },
      [this, coc, depth, extent, frame](PassContext& ctx) {
        CocPush p{};
        p.size[0] = extent.width;
        p.size[1] = extent.height;
        p.near_plane = frame.near_plane;
        p.aperture = frame.aperture;
        p.max_coc = frame.max_coc;
        p.focus_speed = frame.focus_speed;
        p.focus_override = frame.focus_distance;
        ctx.cmd->BindPipeline(coc_pipeline_);
        ctx.cmd->BindTransient(0, {Bind::Storage(0, ctx.graph->image(coc)),
                                   Bind::Sampled(1, ctx.graph->image(depth)),
                                   Bind::StorageBuffer(2, focus_state_, 0, focus_state_.size)});
        ctx.cmd->Push(p);
        ctx.cmd->Dispatch2D(extent);
      });

  graph.AddPass(
      "dof_gather",
      [&](RenderGraph::PassBuilder& b) {
        b.Write(gathered, ResourceUsage::kStorageWrite);
        b.Read(color, ResourceUsage::kSampledCompute);
        b.Read(coc, ResourceUsage::kSampledCompute);
      },
      [this, gathered, color, coc, half, frame](PassContext& ctx) {
        GatherPush p{};
        p.size[0] = half.width;
        p.size[1] = half.height;
        p.inv_size[0] = 1.0f / static_cast<f32>(half.width);
        p.inv_size[1] = 1.0f / static_cast<f32>(half.height);
        p.max_coc = frame.max_coc;
        ctx.cmd->BindPipeline(gather_pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::Storage(0, ctx.graph->image(gathered)),
                Bind::Combined(1, ctx.graph->image(color).view, sampler_),
                Bind::Combined(2, ctx.graph->image(coc).view, sampler_)});
        ctx.cmd->Push(p);
        ctx.cmd->Dispatch2D(half);
      });

  graph.AddPass(
      "dof_composite",
      [&](RenderGraph::PassBuilder& b) {
        b.Write(out, ResourceUsage::kStorageWrite);
        for (ResourceHandle h : {color, gathered, coc})
          b.Read(h, ResourceUsage::kSampledCompute);
      },
      [this, out, color, gathered, coc, extent](PassContext& ctx) {
        CompositePush p{};
        p.size[0] = extent.width;
        p.size[1] = extent.height;
        p.inv_size[0] = 1.0f / static_cast<f32>(extent.width);
        p.inv_size[1] = 1.0f / static_cast<f32>(extent.height);
        ctx.cmd->BindPipeline(composite_pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::Storage(0, ctx.graph->image(out)),
                Bind::Sampled(1, ctx.graph->image(color)),
                Bind::Combined(2, ctx.graph->image(gathered).view, sampler_),
                Bind::Sampled(3, ctx.graph->image(coc))});
        ctx.cmd->Push(p);
        ctx.cmd->Dispatch2D(extent);
      });
  return out;
}

}  // namespace rec::render
