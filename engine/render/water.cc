#include "render/water.h"

#include "asset/mesh.h"
#include "core/log.h"
#include "render/mesh_pipeline.h"
#include "render/shader_util.h"
#include "shaders/copy_cs_hlsl.h"
#include "shaders/mesh_vs_hlsl.h"
#include "shaders/water_ps_hlsl.h"

namespace rec::render {

std::unique_ptr<WaterPass> WaterPass::Create(Device& device, VkFormat color_format,
                                             VkFormat motion_format, VkFormat depth_format,
                                             VkDescriptorSetLayout globals_layout,
                                             VkDescriptorSetLayout material_layout,
                                             VkDescriptorSetLayout environment_layout,
                                             VkDescriptorSetLayout bindless_layout) {
  auto pass = std::unique_ptr<WaterPass>(new WaterPass(device));

  VkSamplerCreateInfo sampler_info{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (vkCreateSampler(device.device(), &sampler_info, nullptr, &pass->sampler_) != VK_SUCCESS) {
    return nullptr;
  }

  VkDescriptorSetLayoutBinding input_bindings[2]{};
  for (u32 i = 0; i < 2; ++i) {
    input_bindings[i].binding = i;
    input_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    input_bindings[i].descriptorCount = 1;
    input_bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo input_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  input_info.bindingCount = 2;
  input_info.pBindings = input_bindings;
  if (vkCreateDescriptorSetLayout(device.device(), &input_info, nullptr,
                                  &pass->input_set_layout_) != VK_SUCCESS) {
    return nullptr;
  }

  VkPushConstantRange push_range{};
  push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  push_range.size = sizeof(MeshPushConstants);

  VkDescriptorSetLayout set_layouts[5] = {globals_layout, material_layout, environment_layout,
                                          bindless_layout, pass->input_set_layout_};
  VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 5;
  layout_info.pSetLayouts = set_layouts;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push_range;
  if (vkCreatePipelineLayout(device.device(), &layout_info, nullptr, &pass->layout_) !=
      VK_SUCCESS) {
    return nullptr;
  }

  VkShaderModule vert = CreateShaderModule(device.device(), k_mesh_vs_hlsl, sizeof(k_mesh_vs_hlsl));
  VkShaderModule frag =
      CreateShaderModule(device.device(), k_water_ps_hlsl, sizeof(k_water_ps_hlsl));
  if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) return nullptr;

  VkPipelineShaderStageCreateInfo stages[2];
  stages[0] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;
  stages[1].pName = "main";

  VkVertexInputBindingDescription binding{};
  binding.stride = sizeof(asset::Vertex);
  binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  VkVertexInputAttributeDescription attributes[5]{};
  attributes[0] = {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,
                   .offset = offsetof(asset::Vertex, position)};
  attributes[1] = {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,
                   .offset = offsetof(asset::Vertex, normal)};
  attributes[2] = {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                   .offset = offsetof(asset::Vertex, tangent)};
  attributes[3] = {.location = 3, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,
                   .offset = offsetof(asset::Vertex, uv)};
  attributes[4] = {.location = 4, .binding = 0, .format = VK_FORMAT_R8G8B8A8_UNORM,
                   .offset = offsetof(asset::Vertex, color)};
  VkPipelineVertexInputStateCreateInfo vertex_input{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex_input.vertexBindingDescriptionCount = 1;
  vertex_input.pVertexBindingDescriptions = &binding;
  vertex_input.vertexAttributeDescriptionCount = 5;
  vertex_input.pVertexAttributeDescriptions = attributes;

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

  // Water replaces its pixels (refraction samples the snapshot), so it
  // renders opaquely over the scene and writes depth for the post stack.
  VkPipelineDepthStencilStateCreateInfo depth{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable = VK_TRUE;
  depth.depthWriteEnable = VK_TRUE;
  depth.depthCompareOp = VK_COMPARE_OP_GREATER;  // reversed z

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
  info.layout = pass->layout_;
  VkResult result = vkCreateGraphicsPipelines(device.device(), VK_NULL_HANDLE, 1, &info, nullptr,
                                              &pass->pipeline_);
  vkDestroyShaderModule(device.device(), vert, nullptr);
  vkDestroyShaderModule(device.device(), frag, nullptr);
  if (result != VK_SUCCESS) {
    REC_ERROR("water pipeline creation failed");
    return nullptr;
  }

  // Snapshot copy compute.
  VkDescriptorSetLayoutBinding copy_bindings[4]{};
  for (u32 i = 0; i < 4; ++i) {
    copy_bindings[i].binding = i;
    copy_bindings[i].descriptorType =
        i < 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    copy_bindings[i].descriptorCount = 1;
    copy_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo copy_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  copy_info.bindingCount = 4;
  copy_info.pBindings = copy_bindings;
  if (vkCreateDescriptorSetLayout(device.device(), &copy_info, nullptr,
                                  &pass->copy_set_layout_) != VK_SUCCESS) {
    return nullptr;
  }
  VkPipelineLayoutCreateInfo copy_layout_info{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  copy_layout_info.setLayoutCount = 1;
  copy_layout_info.pSetLayouts = &pass->copy_set_layout_;
  if (vkCreatePipelineLayout(device.device(), &copy_layout_info, nullptr, &pass->copy_layout_) !=
      VK_SUCCESS) {
    return nullptr;
  }
  VkShaderModule copy_module =
      CreateShaderModule(device.device(), k_copy_cs_hlsl, sizeof(k_copy_cs_hlsl));
  if (copy_module == VK_NULL_HANDLE) return nullptr;
  VkComputePipelineCreateInfo copy_pipeline_info{
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  copy_pipeline_info.stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  copy_pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  copy_pipeline_info.stage.module = copy_module;
  copy_pipeline_info.stage.pName = "main";
  copy_pipeline_info.layout = pass->copy_layout_;
  result = vkCreateComputePipelines(device.device(), VK_NULL_HANDLE, 1, &copy_pipeline_info,
                                    nullptr, &pass->copy_pipeline_);
  vkDestroyShaderModule(device.device(), copy_module, nullptr);
  if (result != VK_SUCCESS) {
    REC_ERROR("water copy pipeline creation failed");
    return nullptr;
  }
  return pass;
}

WaterPass::~WaterPass() {
  VkDevice device = device_.device();
  if (pipeline_) vkDestroyPipeline(device, pipeline_, nullptr);
  if (layout_) vkDestroyPipelineLayout(device, layout_, nullptr);
  if (input_set_layout_) vkDestroyDescriptorSetLayout(device, input_set_layout_, nullptr);
  if (copy_pipeline_) vkDestroyPipeline(device, copy_pipeline_, nullptr);
  if (copy_layout_) vkDestroyPipelineLayout(device, copy_layout_, nullptr);
  if (copy_set_layout_) vkDestroyDescriptorSetLayout(device, copy_set_layout_, nullptr);
  if (sampler_) vkDestroySampler(device, sampler_, nullptr);
}

void WaterPass::RecordCopy(PassContext& ctx, ResourceHandle scene_color, ResourceHandle depth,
                           ResourceHandle opaque_color, ResourceHandle opaque_depth, u32 width,
                           u32 height) {
  VkDescriptorSet set = ctx.allocate_set(copy_set_layout_);
  VkDescriptorImageInfo images[4]{};
  images[0] = {.imageView = ctx.graph->image(opaque_color).view,
               .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
  images[1] = {.imageView = ctx.graph->image(opaque_depth).view,
               .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
  images[2] = {.sampler = sampler_, .imageView = ctx.graph->image(scene_color).view,
               .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  images[3] = {.sampler = sampler_, .imageView = ctx.graph->image(depth).view,
               .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet writes[4];
  for (u32 i = 0; i < 4; ++i) {
    writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType =
        i < 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[i].pImageInfo = &images[i];
  }
  vkUpdateDescriptorSets(ctx.device->device(), 4, writes, 0, nullptr);

  vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, copy_pipeline_);
  vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, copy_layout_, 0, 1, &set, 0,
                          nullptr);
  vkCmdDispatch(ctx.cmd, (width + 7) / 8, (height + 7) / 8, 1);
}

void WaterPass::Bind(PassContext& ctx, VkDescriptorSet globals, VkDescriptorSet environment,
                     VkDescriptorSet bindless, ResourceHandle opaque_color,
                     ResourceHandle opaque_depth) {
  VkDescriptorSet input_set = ctx.allocate_set(input_set_layout_);
  VkDescriptorImageInfo images[2]{};
  images[0] = {.sampler = sampler_, .imageView = ctx.graph->image(opaque_color).view,
               .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  images[1] = {.sampler = sampler_, .imageView = ctx.graph->image(opaque_depth).view,
               .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet writes[2];
  for (u32 i = 0; i < 2; ++i) {
    writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[i].dstSet = input_set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[i].pImageInfo = &images[i];
  }
  vkUpdateDescriptorSets(ctx.device->device(), 2, writes, 0, nullptr);

  vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
  VkDescriptorSet sets[5] = {globals, VK_NULL_HANDLE, environment, bindless, input_set};
  vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &sets[0], 0,
                          nullptr);
  vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 2, 3, &sets[2], 0,
                          nullptr);
}

void WaterPass::BindMaterial(VkCommandBuffer cmd, VkDescriptorSet material) {
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 1, 1, &material, 0,
                          nullptr);
}

}  // namespace rec::render
