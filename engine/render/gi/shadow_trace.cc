#include "render/gi/shadow_trace.h"

#include <cmath>

#include "core/log.h"
#include "render/gi/denoiser_nrd.h"
#include "render/gi/raytracing.h"
#include "render/rhi/device.h"
#include "render/util/shader_util.h"

// shadow_trace.cs pulls in NRD.hlsli for the SIGMA penumbra packing, so it only
// compiles (and this pass only exists) when NRD is built in.
#if defined(RECREATION_HAS_NRD)
#include "shaders/shadow_trace_cs_hlsl.h"

namespace rec::render {
namespace {

struct ShadowTracePush {
  Mat4 inv_view_proj;
  f32 to_light_x;
  f32 to_light_y;
  f32 to_light_z;
  f32 near_plane;
  f32 inv_size[2];
  f32 tan_angular_radius;
  f32 max_distance;
  f32 jitter[2];
};

}  // namespace

bool ShadowTracePass::Initialize(Device& device) {
  VkDescriptorSetLayoutBinding bindings[3]{};
  bindings[0].binding = 0;  // penumbra out
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  bindings[1].binding = 1;  // depth
  bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  bindings[1].descriptorCount = 1;
  bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  bindings[2].binding = 2;  // tlas
  bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
  bindings[2].descriptorCount = 1;
  bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutCreateInfo set_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 3;
  set_info.pBindings = bindings;
  if (vkCreateDescriptorSetLayout(device.device(), &set_info, nullptr, &set_layout_) != VK_SUCCESS) {
    return false;
  }

  VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ShadowTracePush)};
  VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push;
  if (vkCreatePipelineLayout(device.device(), &layout_info, nullptr, &layout_) != VK_SUCCESS) {
    return false;
  }

  VkShaderModule module =
      CreateShaderModule(device.device(), k_shadow_trace_cs_hlsl, sizeof(k_shadow_trace_cs_hlsl));
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
    REC_ERROR("shadow trace pipeline creation failed");
    return false;
  }
  return true;
}

void ShadowTracePass::Destroy(Device& device) {
  if (pipeline_) vkDestroyPipeline(device.device(), pipeline_, nullptr);
  if (layout_) vkDestroyPipelineLayout(device.device(), layout_, nullptr);
  if (set_layout_) vkDestroyDescriptorSetLayout(device.device(), set_layout_, nullptr);
  pipeline_ = VK_NULL_HANDLE;
  layout_ = VK_NULL_HANDLE;
  set_layout_ = VK_NULL_HANDLE;
}

ResourceHandle ShadowTracePass::AddToGraph(RenderGraph& graph, RayTracingContext& raytracing,
                                           u32 tlas_slot, ResourceHandle depth,
                                           const Mat4& inv_view_proj, const Vec3& sun_direction,
                                           f32 near_plane, f32 angular_radius, f32 jitter_x,
                                           f32 jitter_y) {
  ResourceHandle penumbra =
      graph.CreateTexture({.name = "shadow_penumbra", .format = NrdDenoiser::kPenumbraFormat,
                           .width = extent_.width, .height = extent_.height});

  Vec3 to_light = Normalize(sun_direction * -1.0f);
  graph.AddPass(
      "shadow_trace",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(depth, ResourceUsage::kSampledCompute);
        builder.Write(penumbra, ResourceUsage::kStorageWrite);
      },
      [this, &raytracing, tlas_slot, depth, penumbra, inv_view_proj, to_light, near_plane,
       angular_radius, jitter_x, jitter_y](PassContext& ctx) {
        VkDescriptorSet set = ctx.allocate_set(set_layout_);

        VkDescriptorImageInfo images[2]{};
        images[0] = {.imageView = ctx.graph->image(penumbra).view,
                     .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        images[1] = {.imageView = ctx.graph->image(depth).view,
                     .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

        VkAccelerationStructureKHR tlas = raytracing.tlas(tlas_slot);
        VkWriteDescriptorSetAccelerationStructureKHR tlas_info{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        tlas_info.accelerationStructureCount = 1;
        tlas_info.pAccelerationStructures = &tlas;

        VkWriteDescriptorSet writes[3];
        writes[0] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &images[0];
        writes[1] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[1].dstSet = set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[1].pImageInfo = &images[1];
        writes[2] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[2].pNext = &tlas_info;
        writes[2].dstSet = set;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        vkUpdateDescriptorSets(ctx.device->device(), 3, writes, 0, nullptr);

        ShadowTracePush push{};
        push.inv_view_proj = inv_view_proj;
        push.to_light_x = to_light.x;
        push.to_light_y = to_light.y;
        push.to_light_z = to_light.z;
        push.near_plane = near_plane;
        push.inv_size[0] = 1.0f / static_cast<f32>(extent_.width);
        push.inv_size[1] = 1.0f / static_cast<f32>(extent_.height);
        push.tan_angular_radius = std::tan(angular_radius > 0.0f ? angular_radius : 0.0045f);
        push.max_distance = 1000.0f;
        push.jitter[0] = jitter_x;
        push.jitter[1] = jitter_y;

        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(ctx.cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(ctx.cmd, (extent_.width + 7) / 8, (extent_.height + 7) / 8, 1);
      });
  return penumbra;
}

}  // namespace rec::render

#endif  // RECREATION_HAS_NRD
