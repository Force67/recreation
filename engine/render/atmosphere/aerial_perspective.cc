#include "render/atmosphere/aerial_perspective.h"

#include "core/log.h"
#include "render/rhi/device.h"
#include "render/util/shader_util.h"
#include "shaders/aerial_perspective_cs_hlsl.h"

namespace rec::render {
namespace {

struct ApPush {
  Mat4 inv_view_proj;
  f32 camera_pos[4];     // xyz eye, w strength
  f32 sun_direction[4];  // xyz travel dir, w intensity
  f32 sun_color[4];      // rgb
  u32 size[2];
  u32 steps;
  u32 pad;
};

}  // namespace

bool AerialPerspective::Initialize(Device& device) {
  VkSamplerCreateInfo sampler_info{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (vkCreateSampler(device.device(), &sampler_info, nullptr, &sampler_) != VK_SUCCESS) {
    return false;
  }

  VkDescriptorSetLayoutBinding bindings[5]{};
  bindings[0] = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                 .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
  bindings[1] = {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                 .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
  bindings[2] = {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                 .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
  bindings[3] = {.binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                 .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
  bindings[4] = {.binding = 4, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                 .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};

  VkDescriptorSetLayoutCreateInfo set_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 5;
  set_info.pBindings = bindings;
  if (vkCreateDescriptorSetLayout(device.device(), &set_info, nullptr, &set_layout_) !=
      VK_SUCCESS) {
    return false;
  }

  VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ApPush)};
  VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push;
  if (vkCreatePipelineLayout(device.device(), &layout_info, nullptr, &layout_) != VK_SUCCESS) {
    return false;
  }

  VkShaderModule module =
      CreateShaderModule(device.device(), k_aerial_perspective_cs_hlsl,
                         sizeof(k_aerial_perspective_cs_hlsl));
  if (module == VK_NULL_HANDLE) return false;
  VkComputePipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  info.stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  info.stage.module = module;
  info.stage.pName = "main";
  info.layout = layout_;
  VkResult result =
      vkCreateComputePipelines(device.device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_);
  vkDestroyShaderModule(device.device(), module, nullptr);
  if (result != VK_SUCCESS) {
    REC_ERROR("aerial perspective pipeline creation failed");
    return false;
  }
  return true;
}

void AerialPerspective::Destroy(Device& device) {
  if (pipeline_) vkDestroyPipeline(device.device(), pipeline_, nullptr);
  if (layout_) vkDestroyPipelineLayout(device.device(), layout_, nullptr);
  if (set_layout_) vkDestroyDescriptorSetLayout(device.device(), set_layout_, nullptr);
  if (sampler_) vkDestroySampler(device.device(), sampler_, nullptr);
  pipeline_ = VK_NULL_HANDLE;
  layout_ = VK_NULL_HANDLE;
  set_layout_ = VK_NULL_HANDLE;
  sampler_ = VK_NULL_HANDLE;
}

ResourceHandle AerialPerspective::AddToGraph(RenderGraph& graph, ResourceHandle color,
                                             ResourceHandle depth, VkImageView transmittance,
                                             VkImageView multiscatter, VkExtent2D extent,
                                             const Frame& frame) {
  ResourceHandle out = graph.CreateTexture({.name = "aerial_perspective",
                                            .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                            .width = extent.width,
                                            .height = extent.height});
  graph.AddPass(
      "aerial_perspective",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(color, ResourceUsage::kSampledCompute);
        builder.Read(depth, ResourceUsage::kSampledCompute);
        builder.Write(out, ResourceUsage::kStorageWrite);
      },
      [this, color, depth, out, transmittance, multiscatter, extent, frame](PassContext& ctx) {
        VkDescriptorSet set = ctx.allocate_set(set_layout_);

        VkDescriptorImageInfo out_info{.imageView = ctx.graph->image(out).view,
                                       .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo color_info{.imageView = ctx.graph->image(color).view,
                                         .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo depth_info{.imageView = ctx.graph->image(depth).view,
                                         .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo tr_info{.sampler = sampler_, .imageView = transmittance,
                                      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo ms_info{.sampler = sampler_, .imageView = multiscatter,
                                      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

        VkWriteDescriptorSet writes[5];
        VkDescriptorType types[5] = {
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
        VkDescriptorImageInfo* infos[5] = {&out_info, &color_info, &depth_info, &tr_info, &ms_info};
        for (u32 i = 0; i < 5; ++i) {
          writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          writes[i].dstSet = set;
          writes[i].dstBinding = i;
          writes[i].descriptorCount = 1;
          writes[i].descriptorType = types[i];
          writes[i].pImageInfo = infos[i];
        }
        vkUpdateDescriptorSets(ctx.device->device(), 5, writes, 0, nullptr);

        ApPush push{};
        push.inv_view_proj = frame.inv_view_proj;
        push.camera_pos[0] = frame.camera_pos.x;
        push.camera_pos[1] = frame.camera_pos.y;
        push.camera_pos[2] = frame.camera_pos.z;
        push.camera_pos[3] = frame.strength;
        Vec3 sun = Normalize(frame.sun_direction);
        push.sun_direction[0] = sun.x;
        push.sun_direction[1] = sun.y;
        push.sun_direction[2] = sun.z;
        push.sun_direction[3] = frame.sun_intensity;
        push.sun_color[0] = frame.sun_color.x;
        push.sun_color[1] = frame.sun_color.y;
        push.sun_color[2] = frame.sun_color.z;
        push.size[0] = extent.width;
        push.size[1] = extent.height;
        push.steps = frame.steps;

        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(ctx.cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(ctx.cmd, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
      });
  return out;
}

}  // namespace rec::render
