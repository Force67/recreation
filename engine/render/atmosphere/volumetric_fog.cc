#include "render/atmosphere/volumetric_fog.h"

#include "core/log.h"
#include "render/gi/raytracing.h"
#include "render/rhi/device.h"
#include "render/util/shader_util.h"
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
  VkDescriptorSetLayoutBinding bindings[4]{};
  bindings[0] = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                 .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
  bindings[1] = {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                 .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
  bindings[2] = {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                 .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
  bindings[3] = {.binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                 .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};

  VkDescriptorSetLayoutCreateInfo set_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 4;
  set_info.pBindings = bindings;
  if (vkCreateDescriptorSetLayout(device.device(), &set_info, nullptr, &set_layout_) !=
      VK_SUCCESS) {
    return false;
  }

  VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FogPush)};
  VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push;
  if (vkCreatePipelineLayout(device.device(), &layout_info, nullptr, &layout_) != VK_SUCCESS) {
    return false;
  }

  VkShaderModule module = CreateShaderModule(device.device(), k_fog_cs_hlsl, sizeof(k_fog_cs_hlsl));
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
    REC_ERROR("volumetric fog pipeline creation failed");
    return false;
  }
  return true;
}

void VolumetricFog::Destroy(Device& device) {
  if (pipeline_) vkDestroyPipeline(device.device(), pipeline_, nullptr);
  if (layout_) vkDestroyPipelineLayout(device.device(), layout_, nullptr);
  if (set_layout_) vkDestroyDescriptorSetLayout(device.device(), set_layout_, nullptr);
  pipeline_ = VK_NULL_HANDLE;
  layout_ = VK_NULL_HANDLE;
  set_layout_ = VK_NULL_HANDLE;
}

ResourceHandle VolumetricFog::AddToGraph(RenderGraph& graph, RayTracingContext& raytracing,
                                         u32 tlas_slot, ResourceHandle color, ResourceHandle depth,
                                         VkExtent2D extent, const Frame& frame) {
  ResourceHandle fogged = graph.CreateTexture({.name = "fogged",
                                               .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                               .width = extent.width, .height = extent.height});
  graph.AddPass(
      "volumetric_fog",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(color, ResourceUsage::kSampledCompute);
        builder.Read(depth, ResourceUsage::kSampledCompute);
        builder.Write(fogged, ResourceUsage::kStorageWrite);
      },
      [this, &raytracing, tlas_slot, color, depth, fogged, extent, frame](PassContext& ctx) {
        VkDescriptorSet set = ctx.allocate_set(set_layout_);

        VkDescriptorImageInfo out_info{.imageView = ctx.graph->image(fogged).view,
                                       .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo color_info{.imageView = ctx.graph->image(color).view,
                                         .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo depth_info{.imageView = ctx.graph->image(depth).view,
                                         .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkAccelerationStructureKHR tlas = raytracing.tlas(tlas_slot);
        VkWriteDescriptorSetAccelerationStructureKHR tlas_info{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        tlas_info.accelerationStructureCount = 1;
        tlas_info.pAccelerationStructures = &tlas;

        VkWriteDescriptorSet writes[4];
        writes[0] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &out_info;
        writes[1] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[1].dstSet = set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[1].pImageInfo = &color_info;
        writes[2] = writes[1];
        writes[2].dstBinding = 2;
        writes[2].pImageInfo = &depth_info;
        writes[3] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[3].pNext = &tlas_info;
        writes[3].dstSet = set;
        writes[3].dstBinding = 3;
        writes[3].descriptorCount = 1;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        vkUpdateDescriptorSets(ctx.device->device(), 4, writes, 0, nullptr);

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

        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(ctx.cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(ctx.cmd, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
      });
  return fogged;
}

}  // namespace rec::render
