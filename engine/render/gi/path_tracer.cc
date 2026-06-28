#include "render/gi/path_tracer.h"

#include "core/log.h"
#include "render/gi/raytracing.h"
#include "render/rhi/device.h"
#include "render/util/shader_util.h"
#include "shaders/pathtrace_cs_hlsl.h"
#if defined(RECREATION_HAS_NRD)
#include "shaders/pathtrace_composite_cs_hlsl.h"
#include "shaders/pathtrace_gbuffer_cs_hlsl.h"
#endif

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

#if defined(RECREATION_HAS_NRD)
// Matches PathGbufferPush in pathtrace_gbuffer.cs.hlsl.
struct PathGbufferPush {
  Mat4 inv_view_proj;
  Mat4 view_proj;
  Mat4 prev_view_proj;
  f32 camera_pos[4];
  f32 sun_direction[4];
  f32 sun_color[4];
  u32 spp;  // lighting samples per pixel (size derived from GetDimensions)
  u32 pad0;
  u32 frame_index;
  u32 bounces;
};

struct CompositePush {
  u32 size[2];
};

VkDescriptorSetLayoutBinding StorageImage(u32 binding) {
  return {.binding = binding, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
}
VkDescriptorSetLayoutBinding SampledImage(u32 binding) {
  return {.binding = binding, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
}
VkDescriptorSetLayoutBinding AccelStructure(u32 binding) {
  return {.binding = binding, .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
}
VkDescriptorSetLayoutBinding CombinedSampler(u32 binding) {
  return {.binding = binding, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
}

VkDescriptorSetLayout CreateSetLayout(VkDevice device, const VkDescriptorSetLayoutBinding* bindings,
                                      u32 count) {
  VkDescriptorSetLayoutCreateInfo info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  info.bindingCount = count;
  info.pBindings = bindings;
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  vkCreateDescriptorSetLayout(device, &info, nullptr, &layout);
  return layout;
}

VkPipelineLayout CreatePipelineLayout(VkDevice device, const VkDescriptorSetLayout* sets,
                                      u32 set_count, u32 push_size) {
  VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size};
  VkPipelineLayoutCreateInfo info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  info.setLayoutCount = set_count;
  info.pSetLayouts = sets;
  info.pushConstantRangeCount = 1;
  info.pPushConstantRanges = &push;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  vkCreatePipelineLayout(device, &info, nullptr, &layout);
  return layout;
}

bool CreatePipeline(VkDevice device, VkPipelineLayout layout, const unsigned char* code, size_t size,
                    VkPipeline* out) {
  VkShaderModule module = CreateShaderModule(device, code, size);
  if (module == VK_NULL_HANDLE) return false;
  VkComputePipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  info.stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  info.stage.module = module;
  info.stage.pName = "main";
  info.layout = layout;
  VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, out);
  vkDestroyShaderModule(device, module, nullptr);
  return result == VK_SUCCESS;
}
#endif  // RECREATION_HAS_NRD

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

#if defined(RECREATION_HAS_NRD)
  VkDevice dev = device.device();
  // Denoised gbuffer: 6 NRD-input storage images (0..5), tlas (6), sky (7).
  VkDescriptorSetLayoutBinding gbuf_bindings[] = {
      StorageImage(0), StorageImage(1), StorageImage(2), StorageImage(3),
      StorageImage(4), StorageImage(5), AccelStructure(6), CombinedSampler(7)};
  gbuffer_set_layout_ = CreateSetLayout(dev, gbuf_bindings, 8);
  VkDescriptorSetLayout gbuf_sets[2] = {gbuffer_set_layout_, bindless_layout};
  gbuffer_layout_ = CreatePipelineLayout(dev, gbuf_sets, 2, sizeof(PathGbufferPush));
  if (gbuffer_set_layout_ == VK_NULL_HANDLE || gbuffer_layout_ == VK_NULL_HANDLE) return false;
  if (!CreatePipeline(dev, gbuffer_layout_, k_pathtrace_gbuffer_cs_hlsl,
                      sizeof(k_pathtrace_gbuffer_cs_hlsl), &gbuffer_pipeline_)) {
    REC_ERROR("path tracer gbuffer pipeline creation failed");
    return false;
  }

  // Composite: output storage (0) + denoised/albedo/background sampled (1..3).
  VkDescriptorSetLayoutBinding comp_bindings[] = {StorageImage(0), SampledImage(1), SampledImage(2),
                                                  SampledImage(3)};
  composite_set_layout_ = CreateSetLayout(dev, comp_bindings, 4);
  composite_layout_ = CreatePipelineLayout(dev, &composite_set_layout_, 1, sizeof(CompositePush));
  if (composite_set_layout_ == VK_NULL_HANDLE || composite_layout_ == VK_NULL_HANDLE) return false;
  if (!CreatePipeline(dev, composite_layout_, k_pathtrace_composite_cs_hlsl,
                      sizeof(k_pathtrace_composite_cs_hlsl), &composite_pipeline_)) {
    REC_ERROR("path tracer composite pipeline creation failed");
    return false;
  }
#endif  // RECREATION_HAS_NRD
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
  VkDevice dev = device.device();
  if (accum_.image) device.DestroyImage(accum_);
  for (VkPipeline p : {pipeline_, gbuffer_pipeline_, composite_pipeline_}) {
    if (p) vkDestroyPipeline(dev, p, nullptr);
  }
  for (VkPipelineLayout l : {layout_, gbuffer_layout_, composite_layout_}) {
    if (l) vkDestroyPipelineLayout(dev, l, nullptr);
  }
  for (VkDescriptorSetLayout s : {set_layout_, gbuffer_set_layout_, composite_set_layout_}) {
    if (s) vkDestroyDescriptorSetLayout(dev, s, nullptr);
  }
  pipeline_ = gbuffer_pipeline_ = composite_pipeline_ = VK_NULL_HANDLE;
  layout_ = gbuffer_layout_ = composite_layout_ = VK_NULL_HANDLE;
  set_layout_ = gbuffer_set_layout_ = composite_set_layout_ = VK_NULL_HANDLE;
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

#if defined(RECREATION_HAS_NRD)
void PathTracer::AddGbufferPass(RenderGraph& graph, RayTracingContext& raytracing, u32 tlas_slot,
                                VkDescriptorSet bindless_set, VkImageView sky_view,
                                VkSampler sky_sampler, const GbufferTargets& t, const Frame& frame) {
  graph.AddPass(
      "pathtrace_gbuffer",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Write(t.radiance_hitdist, ResourceUsage::kStorageWrite);
        builder.Write(t.normal_roughness, ResourceUsage::kStorageWrite);
        builder.Write(t.viewz, ResourceUsage::kStorageWrite);
        builder.Write(t.motion, ResourceUsage::kStorageWrite);
        builder.Write(t.albedo, ResourceUsage::kStorageWrite);
        builder.Write(t.background, ResourceUsage::kStorageWrite);
      },
      [this, &raytracing, tlas_slot, bindless_set, sky_view, sky_sampler, t,
       frame](PassContext& ctx) {
        VkDescriptorSet set = ctx.allocate_set(gbuffer_set_layout_);

        ResourceHandle handles[6] = {t.radiance_hitdist, t.normal_roughness, t.viewz,
                                     t.motion, t.albedo, t.background};
        VkDescriptorImageInfo storage_info[6];
        VkWriteDescriptorSet writes[8];
        for (u32 i = 0; i < 6; ++i) {
          storage_info[i] = {.imageView = ctx.graph->image(handles[i]).view,
                             .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
          writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          writes[i].dstSet = set;
          writes[i].dstBinding = i;
          writes[i].descriptorCount = 1;
          writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
          writes[i].pImageInfo = &storage_info[i];
        }
        VkDescriptorImageInfo sky_info{.sampler = sky_sampler, .imageView = sky_view,
                                       .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkAccelerationStructureKHR tlas = raytracing.tlas(tlas_slot);
        VkWriteDescriptorSetAccelerationStructureKHR tlas_info{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        tlas_info.accelerationStructureCount = 1;
        tlas_info.pAccelerationStructures = &tlas;
        writes[6] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[6].pNext = &tlas_info;
        writes[6].dstSet = set;
        writes[6].dstBinding = 6;
        writes[6].descriptorCount = 1;
        writes[6].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        writes[7] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[7].dstSet = set;
        writes[7].dstBinding = 7;
        writes[7].descriptorCount = 1;
        writes[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[7].pImageInfo = &sky_info;
        vkUpdateDescriptorSets(ctx.device->device(), 8, writes, 0, nullptr);

        PathGbufferPush push{};
        push.inv_view_proj = frame.inv_view_proj;
        push.view_proj = frame.view_proj;
        push.prev_view_proj = frame.prev_view_proj;
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
        push.spp = frame.spp < 1 ? 1u : frame.spp;
        push.pad0 = 0;
        push.frame_index = frame.frame_index;
        push.bounces = bounces_;

        VkDescriptorSet sets[2] = {set, bindless_set};
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gbuffer_pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gbuffer_layout_, 0, 2, sets,
                                0, nullptr);
        vkCmdPushConstants(ctx.cmd, gbuffer_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                           &push);
        vkCmdDispatch(ctx.cmd, (extent_.width + 7) / 8, (extent_.height + 7) / 8, 1);
      });
}

void PathTracer::AddCompositePass(RenderGraph& graph, ResourceHandle denoised, ResourceHandle albedo,
                                  ResourceHandle background, ResourceHandle output) {
  graph.AddPass(
      "pathtrace_composite",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(denoised, ResourceUsage::kSampledCompute);
        builder.Read(albedo, ResourceUsage::kSampledCompute);
        builder.Read(background, ResourceUsage::kSampledCompute);
        builder.Write(output, ResourceUsage::kStorageWrite);
      },
      [this, denoised, albedo, background, output](PassContext& ctx) {
        VkDescriptorSet set = ctx.allocate_set(composite_set_layout_);

        VkDescriptorImageInfo output_info{.imageView = ctx.graph->image(output).view,
                                          .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        ResourceHandle reads[3] = {denoised, albedo, background};
        VkDescriptorImageInfo read_info[3];
        VkWriteDescriptorSet writes[4];
        writes[0] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &output_info;
        for (u32 i = 0; i < 3; ++i) {
          read_info[i] = {.imageView = ctx.graph->image(reads[i]).view,
                          .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
          writes[i + 1] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          writes[i + 1].dstSet = set;
          writes[i + 1].dstBinding = i + 1;
          writes[i + 1].descriptorCount = 1;
          writes[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
          writes[i + 1].pImageInfo = &read_info[i];
        }
        vkUpdateDescriptorSets(ctx.device->device(), 4, writes, 0, nullptr);

        CompositePush push{{extent_.width, extent_.height}};
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_layout_, 0, 1,
                                &set, 0, nullptr);
        vkCmdPushConstants(ctx.cmd, composite_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                           &push);
        vkCmdDispatch(ctx.cmd, (extent_.width + 7) / 8, (extent_.height + 7) / 8, 1);
      });
}
#endif  // RECREATION_HAS_NRD

}  // namespace rec::render
