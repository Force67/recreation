#include "render/ssr.h"

#include "core/log.h"
#include "render/rhi/device.h"
#include "render/shader_util.h"
#include "shaders/ssr_cs_hlsl.h"

namespace rec::render {
namespace {

struct SsrPush {
  Mat4 view_proj;
  Mat4 inv_view_proj;
  f32 camera_pos[4];
  f32 inv_size[2];
  f32 intensity;
  f32 max_distance;
  f32 thickness;
  f32 frame_index;
  u32 step_count;
  u32 pad;
};

}  // namespace

bool SsrPass::Initialize(Device& device) {
  // 0: output color (storage), 1: depth, 2: normals, 3: scene color (all sampled).
  VkDescriptorSetLayoutBinding bindings[4]{};
  bindings[0].binding = 0;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  for (u32 i = 1; i <= 3; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }

  VkDescriptorSetLayoutCreateInfo set_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 4;
  set_info.pBindings = bindings;
  if (vkCreateDescriptorSetLayout(device.device(), &set_info, nullptr, &set_layout_) !=
      VK_SUCCESS) {
    return false;
  }

  VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SsrPush)};
  VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push;
  if (vkCreatePipelineLayout(device.device(), &layout_info, nullptr, &layout_) != VK_SUCCESS) {
    return false;
  }

  VkShaderModule module = CreateShaderModule(device.device(), k_ssr_cs_hlsl, sizeof(k_ssr_cs_hlsl));
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
    REC_ERROR("ssr pipeline creation failed");
    return false;
  }
  return true;
}

void SsrPass::Destroy(Device& device) {
  if (pipeline_) vkDestroyPipeline(device.device(), pipeline_, nullptr);
  if (layout_) vkDestroyPipelineLayout(device.device(), layout_, nullptr);
  if (set_layout_) vkDestroyDescriptorSetLayout(device.device(), set_layout_, nullptr);
  pipeline_ = VK_NULL_HANDLE;
  layout_ = VK_NULL_HANDLE;
  set_layout_ = VK_NULL_HANDLE;
}

ResourceHandle SsrPass::AddToGraph(RenderGraph& graph, ResourceHandle scene_color,
                                   ResourceHandle depth, ResourceHandle normals,
                                   const Mat4& view_proj, const Mat4& inv_view_proj,
                                   const Vec3& camera_pos, u32 frame_index) {
  ResourceHandle out = graph.CreateTexture({.name = "ssr",
                                            .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                            .width = extent_.width,
                                            .height = extent_.height});

  graph.AddPass(
      "ssr",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(scene_color, ResourceUsage::kSampledCompute);
        builder.Read(depth, ResourceUsage::kSampledCompute);
        builder.Read(normals, ResourceUsage::kSampledCompute);
        builder.Write(out, ResourceUsage::kStorageWrite);
      },
      [this, scene_color, depth, normals, out, view_proj, inv_view_proj, camera_pos,
       frame_index](PassContext& ctx) {
        VkDescriptorSet set = ctx.allocate_set(set_layout_);

        VkDescriptorImageInfo images[4]{};
        images[0] = {.imageView = ctx.graph->image(out).view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        images[1] = {.imageView = ctx.graph->image(depth).view,
                     .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        images[2] = {.imageView = ctx.graph->image(normals).view,
                     .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        images[3] = {.imageView = ctx.graph->image(scene_color).view,
                     .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

        VkWriteDescriptorSet writes[4];
        writes[0] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &images[0];
        for (u32 i = 1; i <= 3; ++i) {
          writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          writes[i].dstSet = set;
          writes[i].dstBinding = i;
          writes[i].descriptorCount = 1;
          writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
          writes[i].pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(ctx.device->device(), 4, writes, 0, nullptr);

        SsrPush push{};
        push.view_proj = view_proj;
        push.inv_view_proj = inv_view_proj;
        push.camera_pos[0] = camera_pos.x;
        push.camera_pos[1] = camera_pos.y;
        push.camera_pos[2] = camera_pos.z;
        push.inv_size[0] = 1.0f / static_cast<f32>(extent_.width);
        push.inv_size[1] = 1.0f / static_cast<f32>(extent_.height);
        push.intensity = settings_.intensity;
        push.max_distance = settings_.max_distance;
        push.thickness = settings_.thickness;
        push.frame_index = static_cast<f32>(frame_index % 4096);
        push.step_count = settings_.step_count;

        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(ctx.cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(ctx.cmd, (extent_.width + 7) / 8, (extent_.height + 7) / 8, 1);
      });
  return out;
}

}  // namespace rec::render
