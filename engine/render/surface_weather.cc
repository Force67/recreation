#include "render/surface_weather.h"

#include "core/log.h"
#include "render/rhi/device.h"
#include "render/shader_util.h"
#include "shaders/surface_weather_cs_hlsl.h"

namespace rec::render {
namespace {

struct SurfacePush {
  Mat4 inv_view_proj;
  f32 camera_pos[4];  // xyz eye
  f32 params[4];      // wetness, snow, unused, unused
  u32 size[2];
  u32 pad[2];
};

}  // namespace

bool SurfaceWeather::Initialize(Device& device) {
  VkDescriptorSetLayoutBinding bindings[5]{};
  bindings[0] = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                 .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
  bindings[1] = {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                 .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
  bindings[2] = {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                 .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
  bindings[3] = {.binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
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

  VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SurfacePush)};
  VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push;
  if (vkCreatePipelineLayout(device.device(), &layout_info, nullptr, &layout_) != VK_SUCCESS) {
    return false;
  }

  VkShaderModule module = CreateShaderModule(device.device(), k_surface_weather_cs_hlsl,
                                             sizeof(k_surface_weather_cs_hlsl));
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
    REC_ERROR("surface weather pipeline creation failed");
    return false;
  }
  return true;
}

void SurfaceWeather::Destroy(Device& device) {
  if (pipeline_) vkDestroyPipeline(device.device(), pipeline_, nullptr);
  if (layout_) vkDestroyPipelineLayout(device.device(), layout_, nullptr);
  if (set_layout_) vkDestroyDescriptorSetLayout(device.device(), set_layout_, nullptr);
  pipeline_ = VK_NULL_HANDLE;
  layout_ = VK_NULL_HANDLE;
  set_layout_ = VK_NULL_HANDLE;
}

ResourceHandle SurfaceWeather::AddToGraph(RenderGraph& graph, ResourceHandle color,
                                          ResourceHandle normals, ResourceHandle depth,
                                          VkImageView sky_view, VkSampler sky_sampler,
                                          VkExtent2D extent, const Frame& frame) {
  ResourceHandle out = graph.CreateTexture({.name = "surface_weather",
                                            .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                            .width = extent.width,
                                            .height = extent.height});
  graph.AddPass(
      "surface_weather",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(color, ResourceUsage::kSampledCompute);
        builder.Read(normals, ResourceUsage::kSampledCompute);
        builder.Read(depth, ResourceUsage::kSampledCompute);
        builder.Write(out, ResourceUsage::kStorageWrite);
      },
      [this, color, normals, depth, out, sky_view, sky_sampler, extent, frame](PassContext& ctx) {
        VkDescriptorSet set = ctx.allocate_set(set_layout_);
        VkDescriptorImageInfo out_info{.imageView = ctx.graph->image(out).view,
                                       .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo color_info{.imageView = ctx.graph->image(color).view,
                                         .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo normal_info{.imageView = ctx.graph->image(normals).view,
                                          .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo depth_info{.imageView = ctx.graph->image(depth).view,
                                         .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo sky_info{.sampler = sky_sampler, .imageView = sky_view,
                                       .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet writes[5];
        VkDescriptorType types[5] = {
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
        VkDescriptorImageInfo* infos[5] = {&out_info, &color_info, &normal_info, &depth_info,
                                           &sky_info};
        for (u32 i = 0; i < 5; ++i) {
          writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          writes[i].dstSet = set;
          writes[i].dstBinding = i;
          writes[i].descriptorCount = 1;
          writes[i].descriptorType = types[i];
          writes[i].pImageInfo = infos[i];
        }
        vkUpdateDescriptorSets(ctx.device->device(), 5, writes, 0, nullptr);

        SurfacePush push{};
        push.inv_view_proj = frame.inv_view_proj;
        push.camera_pos[0] = frame.camera_pos.x;
        push.camera_pos[1] = frame.camera_pos.y;
        push.camera_pos[2] = frame.camera_pos.z;
        push.params[0] = frame.wetness;
        push.params[1] = frame.snow ? 1.0f : 0.0f;
        push.size[0] = extent.width;
        push.size[1] = extent.height;

        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(ctx.cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(ctx.cmd, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
      });
  return out;
}

}  // namespace rec::render
