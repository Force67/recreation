#include "render/environment.h"

#include <cstring>

#include "core/log.h"
#include "render/shader_util.h"
#include "shaders/brdf_lut_cs_hlsl.h"
#include "shaders/fullscreen_vs_hlsl.h"
#include "shaders/irradiance_cs_hlsl.h"
#include "shaders/prefilter_cs_hlsl.h"
#include "shaders/sky_cs_hlsl.h"
#include "shaders/sky_ps_hlsl.h"

namespace rec::render {
namespace {

struct SkyPush {
  f32 sun_direction[3];
  f32 intensity;
  f32 sun_color[3];
  f32 face_size;
};

struct SizePush {
  f32 size;
};

struct PrefilterPush {
  f32 face_size;
  f32 roughness;
};

void CubeBarrier(VkCommandBuffer cmd, VkImage image, u32 mip_count, VkImageLayout old_layout,
                 VkImageLayout new_layout, VkPipelineStageFlags2 src_stage,
                 VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                 VkAccessFlags2 dst_access) {
  VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  barrier.srcStageMask = src_stage;
  barrier.srcAccessMask = src_access;
  barrier.dstStageMask = dst_stage;
  barrier.dstAccessMask = dst_access;
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_count, 0, 6};
  VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cmd, &dep);
}

}  // namespace

std::unique_ptr<EnvironmentSystem> EnvironmentSystem::Create(Device& device) {
  auto env = std::unique_ptr<EnvironmentSystem>(new EnvironmentSystem(device));

  VkSamplerCreateInfo sampler_info{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.maxLod = VK_LOD_CLAMP_NONE;
  if (vkCreateSampler(device.device(), &sampler_info, nullptr, &env->sampler_) != VK_SUCCESS) {
    return nullptr;
  }

  VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16},
  };
  VkDescriptorPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 16;
  pool_info.poolSizeCount = 2;
  pool_info.pPoolSizes = sizes;
  if (vkCreateDescriptorPool(device.device(), &pool_info, nullptr, &env->pool_) != VK_SUCCESS) {
    return nullptr;
  }

  if (!env->CreateImages() || !env->CreateDummies()) return nullptr;
  if (!env->CreatePipelines()) return nullptr;
  if (!env->BakeBrdfLut()) return nullptr;
  return env;
}

bool EnvironmentSystem::CreateImages() {
  VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
  sky_ = device_.CreateImageCube(VK_FORMAT_R16G16B16A16_SFLOAT, kSkySize, usage);
  irradiance_ = device_.CreateImageCube(VK_FORMAT_R16G16B16A16_SFLOAT, kIrradianceSize, usage);
  prefiltered_ = device_.CreateImageCube(VK_FORMAT_R16G16B16A16_SFLOAT, kPrefilterSize, usage,
                                         kPrefilterMips);
  brdf_lut_ = device_.CreateImage2D(VK_FORMAT_R16G16_SFLOAT, {kBrdfLutSize, kBrdfLutSize}, usage,
                                    VK_IMAGE_ASPECT_COLOR_BIT);
  if (!sky_.image || !irradiance_.image || !prefiltered_.image || !brdf_lut_.image) return false;

  auto make_array_view = [&](VkImage image, u32 mip, VkImageView* out) {
    VkImageViewCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 6};
    return vkCreateImageView(device_.device(), &info, nullptr, out) == VK_SUCCESS;
  };
  if (!make_array_view(sky_.image, 0, &sky_storage_view_)) return false;
  if (!make_array_view(irradiance_.image, 0, &irradiance_storage_view_)) return false;
  for (u32 mip = 0; mip < kPrefilterMips; ++mip) {
    if (!make_array_view(prefiltered_.image, mip, &prefilter_storage_views_[mip])) return false;
  }
  return true;
}

bool EnvironmentSystem::CreateDummies() {
  white_ = device_.CreateImage2D(VK_FORMAT_R8_UNORM, {1, 1},
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT);
  black_array_ = device_.CreateImage2D(VK_FORMAT_R16G16B16A16_SFLOAT, {1, 1},
                                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                       VK_IMAGE_ASPECT_COLOR_BIT);
  if (!white_.image || !black_array_.image) return false;

  VkImageViewCreateInfo view_info{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = black_array_.image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  view_info.format = black_array_.format;
  view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(device_.device(), &view_info, nullptr, &black_array_view_) != VK_SUCCESS) {
    return false;
  }

  dummy_volume_ = device_.CreateBuffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
  if (!dummy_volume_.mapped) return false;
  std::memset(dummy_volume_.mapped, 0, 256);

  device_.ImmediateSubmit([&](VkCommandBuffer cmd) {
    for (GpuImage* image : {&white_, &black_array_}) {
      VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
      barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.image = image->image;
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.imageMemoryBarrierCount = 1;
      dep.pImageMemoryBarriers = &barrier;
      vkCmdPipelineBarrier2(cmd, &dep);

      VkClearColorValue clear{};
      if (image == &white_) clear.float32[0] = 1.0f;
      VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdClearColorImage(cmd, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                           &range);

      barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
      barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      vkCmdPipelineBarrier2(cmd, &dep);
    }
  });
  return true;
}

bool EnvironmentSystem::CreatePipelines() {
  auto make_compute = [&](ComputePass* pass, const unsigned char* spv, size_t spv_size,
                          bool sampled_input, u32 push_size) {
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
    set_info.bindingCount = sampled_input ? 2 : 1;
    set_info.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device_.device(), &set_info, nullptr, &pass->set_layout) !=
        VK_SUCCESS) {
      return false;
    }

    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size};
    VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &pass->set_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device_.device(), &layout_info, nullptr, &pass->layout) !=
        VK_SUCCESS) {
      return false;
    }

    VkShaderModule module = CreateShaderModule(device_.device(), spv, spv_size);
    if (module == VK_NULL_HANDLE) return false;
    VkComputePipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = pass->layout;
    VkResult result = vkCreateComputePipelines(device_.device(), VK_NULL_HANDLE, 1, &info,
                                               nullptr, &pass->pipeline);
    vkDestroyShaderModule(device_.device(), module, nullptr);
    return result == VK_SUCCESS;
  };

  if (!make_compute(&sky_gen_, k_sky_cs_hlsl, sizeof(k_sky_cs_hlsl), false, sizeof(SkyPush)) ||
      !make_compute(&irradiance_gen_, k_irradiance_cs_hlsl, sizeof(k_irradiance_cs_hlsl), true,
                    sizeof(SizePush)) ||
      !make_compute(&prefilter_gen_, k_prefilter_cs_hlsl, sizeof(k_prefilter_cs_hlsl), true,
                    sizeof(PrefilterPush)) ||
      !make_compute(&brdf_gen_, k_brdf_lut_cs_hlsl, sizeof(k_brdf_lut_cs_hlsl), false,
                    sizeof(SizePush))) {
    REC_ERROR("environment compute pipeline creation failed");
    return false;
  }

  auto allocate = [&](VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    info.descriptorPool = pool_;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(device_.device(), &info, &set);
    return set;
  };
  auto write_set = [&](VkDescriptorSet set, VkImageView storage, VkImageView sampled) {
    VkDescriptorImageInfo images[2]{};
    images[0] = {.imageView = storage, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    images[1] = {.sampler = sampler_, .imageView = sampled,
                 .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[2];
    u32 count = sampled ? 2 : 1;
    for (u32 i = 0; i < count; ++i) {
      writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[i].dstSet = set;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType =
          i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(device_.device(), count, writes, 0, nullptr);
  };

  sky_gen_.set = allocate(sky_gen_.set_layout);
  write_set(sky_gen_.set, sky_storage_view_, VK_NULL_HANDLE);
  irradiance_gen_.set = allocate(irradiance_gen_.set_layout);
  write_set(irradiance_gen_.set, irradiance_storage_view_, sky_.view);
  for (u32 mip = 0; mip < kPrefilterMips; ++mip) {
    prefilter_sets_[mip] = allocate(prefilter_gen_.set_layout);
    write_set(prefilter_sets_[mip], prefilter_storage_views_[mip], sky_.view);
  }
  brdf_gen_.set = allocate(brdf_gen_.set_layout);
  write_set(brdf_gen_.set, brdf_lut_.view, VK_NULL_HANDLE);

  // Set 2 of the mesh pipeline: ibl inputs, per frame ao, ddgi atlases.
  VkDescriptorSetLayoutBinding env_bindings[7]{};
  for (u32 i = 0; i < 6; ++i) {
    env_bindings[i].binding = i;
    env_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    env_bindings[i].descriptorCount = 1;
    env_bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  env_bindings[6].binding = 6;
  env_bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  env_bindings[6].descriptorCount = 1;
  env_bindings[6].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo env_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  env_info.bindingCount = 7;
  env_info.pBindings = env_bindings;
  if (vkCreateDescriptorSetLayout(device_.device(), &env_info, nullptr, &env_set_layout_) !=
      VK_SUCCESS) {
    return false;
  }

  return true;
}

bool EnvironmentSystem::CreateSkyPipeline(VkDescriptorSetLayout globals_layout,
                                          VkFormat color_format, VkFormat motion_format,
                                          VkFormat depth_format) {
  // Sky background pipeline.
  VkDescriptorSetLayoutBinding sky_binding{};
  sky_binding.binding = 0;
  sky_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  sky_binding.descriptorCount = 1;
  sky_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo sky_set_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  sky_set_info.bindingCount = 1;
  sky_set_info.pBindings = &sky_binding;
  if (vkCreateDescriptorSetLayout(device_.device(), &sky_set_info, nullptr,
                                  &sky_draw_set_layout_) != VK_SUCCESS) {
    return false;
  }

  VkDescriptorSetLayout sky_layouts[2] = {globals_layout, sky_draw_set_layout_};
  VkPipelineLayoutCreateInfo sky_layout_info{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  sky_layout_info.setLayoutCount = 2;
  sky_layout_info.pSetLayouts = sky_layouts;
  if (vkCreatePipelineLayout(device_.device(), &sky_layout_info, nullptr, &sky_draw_layout_) !=
      VK_SUCCESS) {
    return false;
  }

  VkShaderModule vert =
      CreateShaderModule(device_.device(), k_fullscreen_vs_hlsl, sizeof(k_fullscreen_vs_hlsl));
  VkShaderModule frag = CreateShaderModule(device_.device(), k_sky_ps_hlsl, sizeof(k_sky_ps_hlsl));
  if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) return false;

  VkPipelineShaderStageCreateInfo stages[2];
  stages[0] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;
  stages[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vertex_input{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo input_assembly{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  // Equal against the cleared reversed-z far value: only empty pixels shade.
  VkPipelineDepthStencilStateCreateInfo depth{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable = VK_TRUE;
  depth.depthWriteEnable = VK_FALSE;
  depth.depthCompareOp = VK_COMPARE_OP_EQUAL;

  VkPipelineColorBlendAttachmentState blend_attachments[2]{};
  blend_attachments[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  blend_attachments[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
  VkPipelineColorBlendStateCreateInfo blend{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 2;
  blend.pAttachments = blend_attachments;

  VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;

  VkFormat color_formats[2] = {color_format, motion_format};
  VkPipelineRenderingCreateInfo rendering{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 2;
  rendering.pColorAttachmentFormats = color_formats;
  rendering.depthAttachmentFormat = depth_format;

  VkGraphicsPipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext = &rendering;
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &input_assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = sky_draw_layout_;
  VkResult result = vkCreateGraphicsPipelines(device_.device(), VK_NULL_HANDLE, 1, &info, nullptr,
                                              &sky_draw_pipeline_);
  vkDestroyShaderModule(device_.device(), vert, nullptr);
  vkDestroyShaderModule(device_.device(), frag, nullptr);
  if (result != VK_SUCCESS) {
    REC_ERROR("sky pipeline creation failed");
    return false;
  }

  VkDescriptorSetAllocateInfo set_alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  set_alloc.descriptorPool = pool_;
  set_alloc.descriptorSetCount = 1;
  set_alloc.pSetLayouts = &sky_draw_set_layout_;
  vkAllocateDescriptorSets(device_.device(), &set_alloc, &sky_draw_set_);
  VkDescriptorImageInfo sky_image{sampler_, sky_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet sky_write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  sky_write.dstSet = sky_draw_set_;
  sky_write.dstBinding = 0;
  sky_write.descriptorCount = 1;
  sky_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  sky_write.pImageInfo = &sky_image;
  vkUpdateDescriptorSets(device_.device(), 1, &sky_write, 0, nullptr);
  return true;
}

bool EnvironmentSystem::BakeBrdfLut() {
  device_.ImmediateSubmit([&](VkCommandBuffer cmd) {
    VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.image = brdf_lut_.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);

    SizePush push{static_cast<f32>(kBrdfLutSize)};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, brdf_gen_.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, brdf_gen_.layout, 0, 1,
                            &brdf_gen_.set, 0, nullptr);
    vkCmdPushConstants(cmd, brdf_gen_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, kBrdfLutSize / 8, kBrdfLutSize / 8, 1);

    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &dep);
  });
  return true;
}

void EnvironmentSystem::RecordUpdate(VkCommandBuffer cmd, const Vec3& sun_direction,
                                     f32 sun_intensity, const Vec3& sun_color) {
  VkImageLayout old_layout =
      maps_initialized_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
  VkPipelineStageFlags2 old_stage = maps_initialized_
                                        ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                        : VK_PIPELINE_STAGE_2_NONE;
  maps_initialized_ = true;

  CubeBarrier(cmd, sky_.image, 1, old_layout, VK_IMAGE_LAYOUT_GENERAL, old_stage, VK_ACCESS_2_NONE,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

  SkyPush sky_push{};
  Vec3 dir = Normalize(sun_direction);
  sky_push.sun_direction[0] = dir.x;
  sky_push.sun_direction[1] = dir.y;
  sky_push.sun_direction[2] = dir.z;
  sky_push.intensity = sun_intensity;
  sky_push.sun_color[0] = sun_color.x;
  sky_push.sun_color[1] = sun_color.y;
  sky_push.sun_color[2] = sun_color.z;
  sky_push.face_size = static_cast<f32>(kSkySize);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sky_gen_.pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sky_gen_.layout, 0, 1,
                          &sky_gen_.set, 0, nullptr);
  vkCmdPushConstants(cmd, sky_gen_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(sky_push),
                     &sky_push);
  vkCmdDispatch(cmd, kSkySize / 8, kSkySize / 8, 6);

  CubeBarrier(cmd, sky_.image, 1, VK_IMAGE_LAYOUT_GENERAL,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

  CubeBarrier(cmd, irradiance_.image, 1, old_layout, VK_IMAGE_LAYOUT_GENERAL, old_stage,
              VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  CubeBarrier(cmd, prefiltered_.image, kPrefilterMips, old_layout, VK_IMAGE_LAYOUT_GENERAL,
              old_stage, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

  SizePush irradiance_push{static_cast<f32>(kIrradianceSize)};
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, irradiance_gen_.pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, irradiance_gen_.layout, 0, 1,
                          &irradiance_gen_.set, 0, nullptr);
  vkCmdPushConstants(cmd, irradiance_gen_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(irradiance_push), &irradiance_push);
  vkCmdDispatch(cmd, kIrradianceSize / 8 + 1, kIrradianceSize / 8 + 1, 6);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prefilter_gen_.pipeline);
  for (u32 mip = 0; mip < kPrefilterMips; ++mip) {
    u32 size = kPrefilterSize >> mip;
    PrefilterPush push{static_cast<f32>(size),
                       static_cast<f32>(mip) / static_cast<f32>(kPrefilterMips - 1)};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prefilter_gen_.layout, 0, 1,
                            &prefilter_sets_[mip], 0, nullptr);
    vkCmdPushConstants(cmd, prefilter_gen_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                       &push);
    vkCmdDispatch(cmd, (size + 7) / 8, (size + 7) / 8, 6);
  }

  CubeBarrier(cmd, irradiance_.image, 1, VK_IMAGE_LAYOUT_GENERAL,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  CubeBarrier(cmd, prefiltered_.image, kPrefilterMips, VK_IMAGE_LAYOUT_GENERAL,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void EnvironmentSystem::DrawSky(VkCommandBuffer cmd, VkDescriptorSet globals) {
  // Rebound under the sky layout: the mesh pipeline layout carries push
  // constant ranges this one lacks, which breaks set compatibility.
  VkDescriptorSet sets[2] = {globals, sky_draw_set_};
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_draw_pipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_draw_layout_, 0, 2, sets, 0,
                          nullptr);
  vkCmdDraw(cmd, 3, 1, 0, 0);
}

void EnvironmentSystem::WriteEnvSet(VkDescriptorSet set, VkImageView ao_view,
                                    const DdgiBinding* ddgi) const {
  VkDescriptorImageInfo images[6]{};
  images[0] = {sampler_, irradiance_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  images[1] = {sampler_, prefiltered_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  images[2] = {sampler_, brdf_lut_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  images[3] = {sampler_, ao_view ? ao_view : white_.view,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  images[4] = {sampler_, ddgi ? ddgi->irradiance : black_array_view_,
               ddgi ? ddgi->layout : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  images[5] = {sampler_, ddgi ? ddgi->distance : black_array_view_,
               ddgi ? ddgi->layout : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkDescriptorBufferInfo volume{ddgi ? ddgi->volume : dummy_volume_.buffer, 0,
                                ddgi ? ddgi->volume_size : 256};

  VkWriteDescriptorSet writes[7];
  for (u32 i = 0; i < 6; ++i) {
    writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[i].pImageInfo = &images[i];
  }
  writes[6] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  writes[6].dstSet = set;
  writes[6].dstBinding = 6;
  writes[6].descriptorCount = 1;
  writes[6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  writes[6].pBufferInfo = &volume;
  vkUpdateDescriptorSets(device_.device(), 7, writes, 0, nullptr);
}

void EnvironmentSystem::DestroyComputePass(ComputePass& pass) {
  if (pass.pipeline) vkDestroyPipeline(device_.device(), pass.pipeline, nullptr);
  if (pass.layout) vkDestroyPipelineLayout(device_.device(), pass.layout, nullptr);
  if (pass.set_layout) vkDestroyDescriptorSetLayout(device_.device(), pass.set_layout, nullptr);
  pass = {};
}

EnvironmentSystem::~EnvironmentSystem() {
  VkDevice device = device_.device();
  if (sky_draw_pipeline_) vkDestroyPipeline(device, sky_draw_pipeline_, nullptr);
  if (sky_draw_layout_) vkDestroyPipelineLayout(device, sky_draw_layout_, nullptr);
  if (sky_draw_set_layout_) vkDestroyDescriptorSetLayout(device, sky_draw_set_layout_, nullptr);
  if (env_set_layout_) vkDestroyDescriptorSetLayout(device, env_set_layout_, nullptr);
  DestroyComputePass(sky_gen_);
  DestroyComputePass(irradiance_gen_);
  DestroyComputePass(prefilter_gen_);
  DestroyComputePass(brdf_gen_);
  if (sky_storage_view_) vkDestroyImageView(device, sky_storage_view_, nullptr);
  if (irradiance_storage_view_) vkDestroyImageView(device, irradiance_storage_view_, nullptr);
  for (VkImageView view : prefilter_storage_views_) {
    if (view) vkDestroyImageView(device, view, nullptr);
  }
  if (black_array_view_) vkDestroyImageView(device, black_array_view_, nullptr);
  device_.DestroyImage(sky_);
  device_.DestroyImage(irradiance_);
  device_.DestroyImage(prefiltered_);
  device_.DestroyImage(brdf_lut_);
  device_.DestroyImage(white_);
  device_.DestroyImage(black_array_);
  device_.DestroyBuffer(dummy_volume_);
  if (pool_) vkDestroyDescriptorPool(device, pool_, nullptr);
  if (sampler_) vkDestroySampler(device, sampler_, nullptr);
}

}  // namespace rec::render
