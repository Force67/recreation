#include "render/post/bloom.h"

#include <algorithm>

#include "core/log.h"
#include "render/rhi/device.h"
#include "render/util/shader_util.h"
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
  VkSamplerCreateInfo sampler_info{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  vkCreateSampler(device.device(), &sampler_info, nullptr, &sampler_);

  VkDescriptorSetLayoutBinding bindings[2]{};
  bindings[0].binding = 0;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  bindings[1].binding = 1;
  bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  bindings[1].descriptorCount = 1;
  bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo set_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 2;
  set_info.pBindings = bindings;
  if (vkCreateDescriptorSetLayout(device.device(), &set_info, nullptr, &set_layout_) !=
      VK_SUCCESS) {
    return false;
  }

  VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DownPush)};
  VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push;
  if (vkCreatePipelineLayout(device.device(), &layout_info, nullptr, &layout_) != VK_SUCCESS) {
    return false;
  }

  auto make = [&](const unsigned char* spv, size_t size, VkPipeline* pipeline) {
    VkShaderModule module = CreateShaderModule(device.device(), spv, size);
    if (module == VK_NULL_HANDLE) return false;
    VkComputePipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = layout_;
    VkResult result =
        vkCreateComputePipelines(device.device(), VK_NULL_HANDLE, 1, &info, nullptr, pipeline);
    vkDestroyShaderModule(device.device(), module, nullptr);
    return result == VK_SUCCESS;
  };
  if (!make(k_bloom_down_cs_hlsl, sizeof(k_bloom_down_cs_hlsl), &down_pipeline_) ||
      !make(k_bloom_up_cs_hlsl, sizeof(k_bloom_up_cs_hlsl), &up_pipeline_)) {
    REC_ERROR("bloom pipeline creation failed");
    return false;
  }
  return true;
}

void BloomPass::Destroy(Device& device) {
  if (down_pipeline_) vkDestroyPipeline(device.device(), down_pipeline_, nullptr);
  if (up_pipeline_) vkDestroyPipeline(device.device(), up_pipeline_, nullptr);
  if (layout_) vkDestroyPipelineLayout(device.device(), layout_, nullptr);
  if (set_layout_) vkDestroyDescriptorSetLayout(device.device(), set_layout_, nullptr);
  if (sampler_) vkDestroySampler(device.device(), sampler_, nullptr);
  down_pipeline_ = VK_NULL_HANDLE;
  up_pipeline_ = VK_NULL_HANDLE;
  layout_ = VK_NULL_HANDLE;
  set_layout_ = VK_NULL_HANDLE;
  sampler_ = VK_NULL_HANDLE;
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
                                   .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                   .width = mip_width,
                                   .height = mip_height});
  }

  auto dispatch = [this](PassContext& ctx, VkPipeline pipeline, ResourceHandle dst,
                         ResourceHandle src, u32 dst_width, u32 dst_height, f32 src_width,
                         f32 src_height, bool first) {
    VkDescriptorSet set = ctx.allocate_set(set_layout_);
    VkDescriptorImageInfo images[2]{};
    images[0] = {.imageView = ctx.graph->image(dst).view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    images[1] = {.sampler = sampler_, .imageView = ctx.graph->image(src).view,
                 .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[2];
    for (u32 i = 0; i < 2; ++i) {
      writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[i].dstSet = set;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType =
          i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(ctx.device->device(), 2, writes, 0, nullptr);

    DownPush push{};
    push.src_inv_size[0] = 1.0f / src_width;
    push.src_inv_size[1] = 1.0f / src_height;
    push.first_pass = first ? 1u : 0u;
    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0,
                            nullptr);
    vkCmdPushConstants(ctx.cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(ctx.cmd, (dst_width + 7) / 8, (dst_height + 7) / 8, 1);
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
                                             .format = VK_FORMAT_R16G16B16A16_SFLOAT,
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
