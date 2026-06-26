#include "render/post/exposure.h"

#include <cstring>

#include "core/log.h"
#include "render/rhi/device.h"
#include "render/util/shader_util.h"
#include "shaders/exposure_resolve_cs_hlsl.h"
#include "shaders/histogram_cs_hlsl.h"

namespace rec::render {
namespace {

// Metering range in log2 luminance; bin 0 stays reserved for pure black.
constexpr f32 kMinLogLuma = -10.0f;
constexpr f32 kMaxLogLuma = 14.0f;

struct HistogramPush {
  f32 min_log_luma;
  f32 inv_log_luma_range;
  u32 width;
  u32 height;
};

struct ResolvePush {
  f32 min_log_luma;
  f32 log_luma_range;
  f32 delta_seconds;
  f32 adaptation_speed;
  f32 compensation;
  u32 auto_exposure;
  f32 manual_exposure;
  f32 pixel_count;
};

}  // namespace

bool ExposurePass::Initialize(Device& device) {
  device_ = &device;
  VkSamplerCreateInfo sampler_info{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler_info.magFilter = VK_FILTER_NEAREST;
  sampler_info.minFilter = VK_FILTER_NEAREST;
  vkCreateSampler(device.device(), &sampler_info, nullptr, &sampler_);

  histogram_ = device.CreateBuffer(256 * sizeof(u32),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  exposure_ = device.CreateBuffer(2 * sizeof(f32),
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  if (!histogram_.buffer || !exposure_.buffer) return false;

  auto make = [&](const unsigned char* spv, size_t spv_size, bool with_image, u32 push_size,
                  VkDescriptorSetLayout* set_layout, VkPipelineLayout* layout,
                  VkPipeline* pipeline) {
    VkDescriptorSetLayoutBinding bindings[2]{};
    u32 count = 0;
    if (with_image) {
      bindings[0] = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
      bindings[1] = {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                     .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
      count = 2;
    } else {
      bindings[0] = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                     .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
      bindings[1] = {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                     .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
      count = 2;
    }
    VkDescriptorSetLayoutCreateInfo set_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = count;
    set_info.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device.device(), &set_info, nullptr, set_layout) !=
        VK_SUCCESS) {
      return false;
    }
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size};
    VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = set_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device.device(), &layout_info, nullptr, layout) != VK_SUCCESS) {
      return false;
    }
    VkShaderModule module = CreateShaderModule(device.device(), spv, spv_size);
    if (module == VK_NULL_HANDLE) return false;
    VkComputePipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = *layout;
    VkResult result =
        vkCreateComputePipelines(device.device(), VK_NULL_HANDLE, 1, &info, nullptr, pipeline);
    vkDestroyShaderModule(device.device(), module, nullptr);
    return result == VK_SUCCESS;
  };

  if (!make(k_histogram_cs_hlsl, sizeof(k_histogram_cs_hlsl), true, sizeof(HistogramPush),
            &histogram_set_layout_, &histogram_layout_, &histogram_pipeline_) ||
      !make(k_exposure_resolve_cs_hlsl, sizeof(k_exposure_resolve_cs_hlsl), false,
            sizeof(ResolvePush), &resolve_set_layout_, &resolve_layout_, &resolve_pipeline_)) {
    REC_ERROR("exposure pipeline creation failed");
    return false;
  }
  return true;
}

void ExposurePass::Destroy(Device& device) {
  device.DestroyBuffer(histogram_);
  device.DestroyBuffer(exposure_);
  if (histogram_pipeline_) vkDestroyPipeline(device.device(), histogram_pipeline_, nullptr);
  if (histogram_layout_) vkDestroyPipelineLayout(device.device(), histogram_layout_, nullptr);
  if (histogram_set_layout_) {
    vkDestroyDescriptorSetLayout(device.device(), histogram_set_layout_, nullptr);
  }
  if (resolve_pipeline_) vkDestroyPipeline(device.device(), resolve_pipeline_, nullptr);
  if (resolve_layout_) vkDestroyPipelineLayout(device.device(), resolve_layout_, nullptr);
  if (resolve_set_layout_) {
    vkDestroyDescriptorSetLayout(device.device(), resolve_set_layout_, nullptr);
  }
  if (sampler_) vkDestroySampler(device.device(), sampler_, nullptr);
  histogram_pipeline_ = VK_NULL_HANDLE;
  histogram_layout_ = VK_NULL_HANDLE;
  histogram_set_layout_ = VK_NULL_HANDLE;
  resolve_pipeline_ = VK_NULL_HANDLE;
  resolve_layout_ = VK_NULL_HANDLE;
  resolve_set_layout_ = VK_NULL_HANDLE;
  sampler_ = VK_NULL_HANDLE;
}

void ExposurePass::AddToGraph(RenderGraph& graph, ResourceHandle input, u32 width, u32 height,
                              f32 delta_seconds) {
  bool first = first_frame_;
  first_frame_ = false;

  graph.AddPass(
      "exposure",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(input, ResourceUsage::kSampledCompute);
      },
      [this, input, width, height, delta_seconds, first](PassContext& ctx) {
        if (first) {
          // Known starting state for both buffers.
          vkCmdFillBuffer(ctx.cmd, histogram_.buffer, 0, histogram_.size, 0);
          vkCmdFillBuffer(ctx.cmd, exposure_.buffer, 0, exposure_.size, 0);
        }
        VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = first ? VK_PIPELINE_STAGE_2_TRANSFER_BIT
                                     : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.srcAccessMask = first ? VK_ACCESS_2_TRANSFER_WRITE_BIT
                                      : VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                                            VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask =
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(ctx.cmd, &dep);

        // Histogram.
        VkDescriptorSet histogram_set = ctx.allocate_set(histogram_set_layout_);
        VkDescriptorImageInfo image{sampler_, ctx.graph->image(input).view,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo histogram_info{histogram_.buffer, 0, histogram_.size};
        VkWriteDescriptorSet writes[2];
        writes[0] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[0].dstSet = histogram_set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &image;
        writes[1] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[1].dstSet = histogram_set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &histogram_info;
        vkUpdateDescriptorSets(ctx.device->device(), 2, writes, 0, nullptr);

        HistogramPush histogram_push{};
        histogram_push.min_log_luma = kMinLogLuma;
        histogram_push.inv_log_luma_range = 1.0f / (kMaxLogLuma - kMinLogLuma);
        histogram_push.width = width;
        histogram_push.height = height;
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, histogram_pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, histogram_layout_, 0, 1,
                                &histogram_set, 0, nullptr);
        vkCmdPushConstants(ctx.cmd, histogram_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(histogram_push), &histogram_push);
        vkCmdDispatch(ctx.cmd, (width + 15) / 16, (height + 15) / 16, 1);

        vkCmdPipelineBarrier2(ctx.cmd, &dep);

        // Resolve + adaptation.
        VkDescriptorSet resolve_set = ctx.allocate_set(resolve_set_layout_);
        VkDescriptorBufferInfo exposure_info{exposure_.buffer, 0, exposure_.size};
        writes[0] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[0].dstSet = resolve_set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo = &histogram_info;
        writes[1] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[1].dstSet = resolve_set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &exposure_info;
        vkUpdateDescriptorSets(ctx.device->device(), 2, writes, 0, nullptr);

        ResolvePush resolve_push{};
        resolve_push.min_log_luma = kMinLogLuma;
        resolve_push.log_luma_range = kMaxLogLuma - kMinLogLuma;
        resolve_push.delta_seconds = delta_seconds;
        resolve_push.adaptation_speed = settings_.adaptation_speed;
        resolve_push.compensation = settings_.compensation;
        resolve_push.auto_exposure = settings_.automatic ? 1u : 0u;
        resolve_push.manual_exposure = settings_.manual_exposure;
        resolve_push.pixel_count = static_cast<f32>(width) * static_cast<f32>(height);
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, resolve_pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, resolve_layout_, 0, 1,
                                &resolve_set, 0, nullptr);
        vkCmdPushConstants(ctx.cmd, resolve_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(resolve_push), &resolve_push);
        vkCmdDispatch(ctx.cmd, 1, 1, 1);

        // Visible to the tonemap fragment shader.
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        vkCmdPipelineBarrier2(ctx.cmd, &dep);
      });
}

}  // namespace rec::render
