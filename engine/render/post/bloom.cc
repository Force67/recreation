#include "render/post/bloom.h"

#include <algorithm>

#include "core/log.h"
#include "render/rhi/device.h"
#include "shaders/bloom_down_cs_hlsl.h"
#include "shaders/bloom_up_cs_hlsl.h"

namespace rec::render {
namespace {

struct DownPush {
  f32 src_inv_size[2];
  u32 first_pass;
  f32 pad;
};

struct UpPush {
  f32 src_inv_size[2];
  f32 pad[2];
};

}  // namespace

bool BloomPass::Initialize(Device& device) {
  sampler_ = device.GetSampler({.address_u = AddressMode::kClampToEdge,
                                .address_v = AddressMode::kClampToEdge,
                                .address_w = AddressMode::kClampToEdge});

  auto make = [&](ShaderBlob shader, const char* name, PipelineHandle* pipeline) {
    *pipeline = device.CreateComputePipeline({
        .shader = shader,
        .sets = {{.slots = {{0, BindingType::kStorageImage},
                            {1, BindingType::kCombinedTextureSampler}}}},
        .push_constant_size = sizeof(DownPush),
        .debug_name = name,
    });
    return static_cast<bool>(*pipeline);
  };
  if (!make(REC_SHADER(k_bloom_down_cs_hlsl), "bloom_down", &down_pipeline_) ||
      !make(REC_SHADER(k_bloom_up_cs_hlsl), "bloom_up", &up_pipeline_)) {
    REC_ERROR("bloom pipeline creation failed");
    return false;
  }
  return true;
}

void BloomPass::Destroy(Device& device) {
  device.DestroyPipeline(down_pipeline_);
  device.DestroyPipeline(up_pipeline_);
  down_pipeline_ = {};
  up_pipeline_ = {};
  sampler_ = {};
}

ResourceHandle BloomPass::AddToGraph(RenderGraph& graph, ResourceHandle input, u32 width,
                                     u32 height) {
  ResourceHandle mips[kMips];
  u32 widths[kMips];
  u32 heights[kMips];
  u32 mip_width = width;
  u32 mip_height = height;
  for (u32 i = 0; i < kMips; ++i) {
    mip_width = std::max(1u, mip_width / 2);
    mip_height = std::max(1u, mip_height / 2);
    widths[i] = mip_width;
    heights[i] = mip_height;
    mips[i] = graph.CreateTexture({.name = "bloom_mip",
                                   .format = Format::kRGBA16Float,
                                   .width = mip_width,
                                   .height = mip_height});
  }

  auto dispatch = [this](PassContext& ctx, PipelineHandle pipeline, ResourceHandle dst,
                         ResourceHandle src, u32 dst_width, u32 dst_height, f32 src_width,
                         f32 src_height, bool first) {
    DownPush push{};
    push.src_inv_size[0] = 1.0f / src_width;
    push.src_inv_size[1] = 1.0f / src_height;
    push.first_pass = first ? 1u : 0u;
    ctx.cmd->BindPipeline(pipeline);
    ctx.cmd->BindTransient(0, {Bind::Storage(0, ctx.graph->image(dst)),
                               Bind::Combined(1, ctx.graph->image(src).view, sampler_)});
    ctx.cmd->Push(push);
    ctx.cmd->Dispatch2D({dst_width, dst_height});
  };

  for (u32 i = 0; i < kMips; ++i) {
    ResourceHandle src = i == 0 ? input : mips[i - 1];
    f32 src_width = static_cast<f32>(i == 0 ? width : widths[i - 1]);
    f32 src_height = static_cast<f32>(i == 0 ? height : heights[i - 1]);
    graph.AddPass(
        "bloom_down",
        [&](RenderGraph::PassBuilder& builder) {
          builder.Read(src, ResourceUsage::kSampledCompute);
          builder.Write(mips[i], ResourceUsage::kStorageWrite);
        },
        [this, dispatch, src, dst = mips[i], dst_width = widths[i], dst_height = heights[i],
         src_width, src_height, first = i == 0](PassContext& ctx) {
          dispatch(ctx, down_pipeline_, dst, src, dst_width, dst_height, src_width, src_height,
                   first);
        });
  }

  for (u32 i = kMips - 1; i > 0; --i) {
    graph.AddPass(
        "bloom_up",
        [&](RenderGraph::PassBuilder& builder) {
          builder.Read(mips[i], ResourceUsage::kSampledCompute);
          builder.Write(mips[i - 1], ResourceUsage::kStorageWrite);
        },
        [this, dispatch, src = mips[i], dst = mips[i - 1], dst_width = widths[i - 1],
         dst_height = heights[i - 1], src_width = static_cast<f32>(widths[i]),
         src_height = static_cast<f32>(heights[i])](PassContext& ctx) {
          dispatch(ctx, up_pipeline_, dst, src, dst_width, dst_height, src_width, src_height,
                   false);
        });
  }

  // Final tent up to full resolution.
  ResourceHandle full = graph.CreateTexture({.name = "bloom",
                                             .format = Format::kRGBA16Float,
                                             .width = width,
                                             .height = height});
  graph.AddPass(
      "bloom_up",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(mips[0], ResourceUsage::kSampledCompute);
        builder.Write(full, ResourceUsage::kStorageWrite);
      },
      [this, dispatch, src = mips[0], full, width, height,
       src_width = static_cast<f32>(widths[0]),
       src_height = static_cast<f32>(heights[0])](PassContext& ctx) {
        // `first` lands in the up shader's replace flag: fresh target.
        dispatch(ctx, up_pipeline_, full, src, width, height, src_width, src_height, true);
      });
  return full;
}

}  // namespace rec::render
