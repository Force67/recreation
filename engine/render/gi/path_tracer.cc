#include "render/gi/path_tracer.h"

#include "core/log.h"
#include "render/gi/raytracing.h"
#include "render/rhi/device.h"
#include "render/util/shader_util.h"
#include "shaders/pathtrace_cs_hlsl.h"

namespace rec::render {
namespace {

struct PathPush {
  Mat4 inv_view_proj;
  f32 camera_pos[4];
  f32 sun_direction[4];
  f32 sun_color[4];
  u32 size[2];
  u32 frame_index;
  u32 sample_base;
  u32 spp;
  u32 bounces;
  u32 reset;
  u32 pad;
};

constexpr VkFormat kAccumFormat = VK_FORMAT_R32G32B32A32_SFLOAT;

}  // namespace

bool PathTracer::Initialize(Device& device, VkDescriptorSetLayout bindless_layout) {
  if (bindless_layout == VK_NULL_HANDLE) return false;

  VkDescriptorSetLayoutBinding bindings[4]{};
  bindings[0] = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                 .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
  bindings[1] = {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                 .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
  bindings[2] = {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                 .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
  bindings[3] = {.binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                 .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};

  VkDescriptorSetLayoutCreateInfo set_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 4;
  set_info.pBindings = bindings;
  if (vkCreateDescriptorSetLayout(device.device(), &set_info, nullptr, &set_layout_) !=
      VK_SUCCESS) {
    return false;
  }

  VkDescriptorSetLayout layouts[2] = {set_layout_, bindless_layout};
  VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PathPush)};
  VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 2;
  layout_info.pSetLayouts = layouts;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push;
  if (vkCreatePipelineLayout(device.device(), &layout_info, nullptr, &layout_) != VK_SUCCESS) {
    return false;
  }

  VkShaderModule module =
      CreateShaderModule(device.device(), k_pathtrace_cs_hlsl, sizeof(k_pathtrace_cs_hlsl));
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
    REC_ERROR("path tracer pipeline creation failed");
    return false;
  }
  return true;
}

void PathTracer::Resize(Device& device, VkExtent2D extent) {
  if (extent.width == extent_.width && extent.height == extent_.height && accum_.image) return;
  if (accum_.image) device.DestroyImage(accum_);
  extent_ = extent;
  accum_ = device.CreateImage2D(kAccumFormat, extent, VK_IMAGE_USAGE_STORAGE_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT);
  accum_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  accumulated_samples_ = 0;
  if (accum_.image == VK_NULL_HANDLE) return;

  // The graph imports the buffer in GENERAL; transition it once up front.
  device.ImmediateSubmit([this](VkCommandBuffer cmd) {
    VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.image = accum_.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
  });
  accum_layout_ = VK_IMAGE_LAYOUT_GENERAL;
}

void PathTracer::Destroy(Device& device) {
  if (accum_.image) device.DestroyImage(accum_);
  if (pipeline_) vkDestroyPipeline(device.device(), pipeline_, nullptr);
  if (layout_) vkDestroyPipelineLayout(device.device(), layout_, nullptr);
  if (set_layout_) vkDestroyDescriptorSetLayout(device.device(), set_layout_, nullptr);
  pipeline_ = VK_NULL_HANDLE;
  layout_ = VK_NULL_HANDLE;
  set_layout_ = VK_NULL_HANDLE;
}

void PathTracer::AddToGraph(RenderGraph& graph, RayTracingContext& raytracing, u32 tlas_slot,
                            VkDescriptorSet bindless_set, VkImageView sky_view,
                            VkSampler sky_sampler, ResourceHandle output, const Frame& frame) {
  if (frame.reset) accumulated_samples_ = 0;
  u32 sample_base = accumulated_samples_;
  accumulated_samples_ += spp_;

  ResourceHandle accum = graph.ImportImage("pt_accum", accum_, &accum_layout_);
  graph.AddPass(
      "pathtrace",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Write(accum, ResourceUsage::kStorageWrite);
        builder.Write(output, ResourceUsage::kStorageWrite);
      },
      [this, &raytracing, tlas_slot, bindless_set, sky_view, sky_sampler, output, accum,
       frame, sample_base](PassContext& ctx) {
        VkDescriptorSet set = ctx.allocate_set(set_layout_);

        VkDescriptorImageInfo output_info{.imageView = ctx.graph->image(output).view,
                                          .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo accum_info{.imageView = ctx.graph->image(accum).view,
                                         .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo sky_info{.sampler = sky_sampler, .imageView = sky_view,
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
        writes[0].pImageInfo = &output_info;
        writes[1] = writes[0];
        writes[1].dstBinding = 1;
        writes[1].pImageInfo = &accum_info;
        writes[2] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[2].pNext = &tlas_info;
        writes[2].dstSet = set;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        writes[3] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[3].dstSet = set;
        writes[3].dstBinding = 3;
        writes[3].descriptorCount = 1;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[3].pImageInfo = &sky_info;
        vkUpdateDescriptorSets(ctx.device->device(), 4, writes, 0, nullptr);

        PathPush push{};
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
        push.sun_color[3] = frame.sun_radius;
        push.size[0] = extent_.width;
        push.size[1] = extent_.height;
        push.frame_index = frame.frame_index;
        push.sample_base = sample_base;
        push.spp = spp_;
        push.bounces = bounces_;
        push.reset = sample_base == 0 ? 1u : 0u;

        VkDescriptorSet sets[2] = {set, bindless_set};
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 2, sets, 0,
                                nullptr);
        vkCmdPushConstants(ctx.cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(ctx.cmd, (extent_.width + 7) / 8, (extent_.height + 7) / 8, 1);
      });
}

}  // namespace rec::render
