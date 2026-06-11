#include "render/antialiasing.h"

#include "core/log.h"
#include "render/rhi/device.h"
#include "render/shader_util.h"
#include "shaders/taa_comp.h"

namespace rec::render {
namespace {

constexpr VkFormat kHistoryFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

struct TaaPushConstants {
  f32 inv_size[2];
  f32 history_blend;
  u32 reset_history;
};

f32 Halton(u32 index, u32 base) {
  f32 result = 0.0f;
  f32 fraction = 1.0f;
  for (u32 i = index + 1; i > 0; i /= base) {
    fraction /= static_cast<f32>(base);
    result += fraction * static_cast<f32>(i % base);
  }
  return result;
}

}  // namespace

void JitterSequence::Sample(u32 frame_index, u32 sample_count, f32* out_x, f32* out_y) {
  u32 index = frame_index % sample_count;
  *out_x = Halton(index, 2) - 0.5f;
  *out_y = Halton(index, 3) - 0.5f;
}

bool TaaPass::Initialize(Device& device) {
  VkSamplerCreateInfo sampler_info{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  vkCreateSampler(device.device(), &sampler_info, nullptr, &sampler_);

  VkDescriptorSetLayoutBinding bindings[4]{};
  bindings[0] = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                 .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
  for (u32 i = 1; i < 4; ++i) {
    bindings[i] = {.binding = i, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                   .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
  }

  VkDescriptorSetLayoutCreateInfo set_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 4;
  set_info.pBindings = bindings;
  if (vkCreateDescriptorSetLayout(device.device(), &set_info, nullptr, &set_layout_) !=
      VK_SUCCESS) {
    return false;
  }

  VkPushConstantRange push_range{};
  push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  push_range.size = sizeof(TaaPushConstants);

  VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push_range;
  if (vkCreatePipelineLayout(device.device(), &layout_info, nullptr, &layout_) != VK_SUCCESS) {
    return false;
  }

  VkShaderModule shader = CreateShaderModule(device.device(), k_taa_comp, sizeof(k_taa_comp));
  if (shader == VK_NULL_HANDLE) {
    REC_ERROR("taa shader module creation failed");
    return false;
  }

  VkComputePipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  info.stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  info.stage.module = shader;
  info.stage.pName = "main";
  info.layout = layout_;
  VkResult result =
      vkCreateComputePipelines(device.device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_);
  vkDestroyShaderModule(device.device(), shader, nullptr);
  if (result != VK_SUCCESS) {
    REC_ERROR("taa pipeline creation failed");
    return false;
  }
  return true;
}

void TaaPass::Resize(Device& device, VkExtent2D extent) {
  for (u32 i = 0; i < 2; ++i) {
    if (history_[i].image) device.DestroyImage(history_[i]);
    history_[i] = device.CreateImage2D(
        kHistoryFormat, extent, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    history_layouts_[i] = VK_IMAGE_LAYOUT_UNDEFINED;
  }
  extent_ = extent;
  history_valid_ = false;
}

void TaaPass::Destroy(Device& device) {
  for (GpuImage& image : history_) {
    if (image.image) device.DestroyImage(image);
  }
  if (pipeline_) vkDestroyPipeline(device.device(), pipeline_, nullptr);
  if (layout_) vkDestroyPipelineLayout(device.device(), layout_, nullptr);
  if (set_layout_) vkDestroyDescriptorSetLayout(device.device(), set_layout_, nullptr);
  if (sampler_) vkDestroySampler(device.device(), sampler_, nullptr);
  pipeline_ = VK_NULL_HANDLE;
  layout_ = VK_NULL_HANDLE;
  set_layout_ = VK_NULL_HANDLE;
  sampler_ = VK_NULL_HANDLE;
}

ResourceHandle TaaPass::AddToGraph(RenderGraph& graph, ResourceHandle color,
                                   ResourceHandle motion, u32 frame_index) {
  u32 write_index = frame_index % 2;
  u32 read_index = 1 - write_index;
  ResourceHandle resolved =
      graph.ImportImage("taa_resolved", history_[write_index], &history_layouts_[write_index]);
  ResourceHandle history =
      graph.ImportImage("taa_history", history_[read_index], &history_layouts_[read_index]);

  bool reset = !history_valid_;
  history_valid_ = true;

  graph.AddPass(
      "taa_resolve",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(color, ResourceUsage::kSampledCompute);
        builder.Read(motion, ResourceUsage::kSampledCompute);
        builder.Read(history, ResourceUsage::kSampledCompute);
        builder.Write(resolved, ResourceUsage::kStorageWrite);
      },
      [this, color, motion, history, resolved, reset](PassContext& ctx) {
        VkDescriptorSet set = ctx.allocate_set(set_layout_);

        VkDescriptorImageInfo images[4]{};
        images[0] = {.imageView = ctx.graph->image(resolved).view,
                     .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        images[1] = {.sampler = sampler_, .imageView = ctx.graph->image(color).view,
                     .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        images[2] = {.sampler = sampler_, .imageView = ctx.graph->image(history).view,
                     .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        images[3] = {.sampler = sampler_, .imageView = ctx.graph->image(motion).view,
                     .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

        VkWriteDescriptorSet writes[4];
        for (u32 i = 0; i < 4; ++i) {
          writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          writes[i].dstSet = set;
          writes[i].dstBinding = i;
          writes[i].descriptorCount = 1;
          writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                            : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
          writes[i].pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(ctx.device->device(), 4, writes, 0, nullptr);

        TaaPushConstants push{};
        push.inv_size[0] = 1.0f / static_cast<f32>(extent_.width);
        push.inv_size[1] = 1.0f / static_cast<f32>(extent_.height);
        push.history_blend = settings_.history_blend;
        push.reset_history = reset ? 1u : 0u;

        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(ctx.cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(ctx.cmd, (extent_.width + 7) / 8, (extent_.height + 7) / 8, 1);
      });
  return resolved;
}

}  // namespace rec::render
