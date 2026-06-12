#include "render/post.h"

#include "core/log.h"
#include "render/shader_util.h"
#include "shaders/fullscreen_vs_hlsl.h"
#include "shaders/tonemap_ps_hlsl.h"

namespace rec::render {

std::unique_ptr<PostPass> PostPass::Create(Device& device, VkFormat output_format) {
  auto pass = std::unique_ptr<PostPass>(new PostPass(device));

  VkSamplerCreateInfo sampler_info{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  vkCreateSampler(device.device(), &sampler_info, nullptr, &pass->sampler_);

  VkDescriptorSetLayoutBinding input_binding{};
  input_binding.binding = 0;
  input_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  input_binding.descriptorCount = 1;
  input_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutCreateInfo set_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 1;
  set_info.pBindings = &input_binding;
  if (vkCreateDescriptorSetLayout(device.device(), &set_info, nullptr, &pass->set_layout_) !=
      VK_SUCCESS) {
    return nullptr;
  }

  VkPushConstantRange push_range{};
  push_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  push_range.size = sizeof(Params);

  VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &pass->set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push_range;
  if (vkCreatePipelineLayout(device.device(), &layout_info, nullptr, &pass->layout_) !=
      VK_SUCCESS) {
    return nullptr;
  }

  VkShaderModule vert =
      CreateShaderModule(device.device(), k_fullscreen_vs_hlsl, sizeof(k_fullscreen_vs_hlsl));
  VkShaderModule frag =
      CreateShaderModule(device.device(), k_tonemap_ps_hlsl, sizeof(k_tonemap_ps_hlsl));
  if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
    REC_ERROR("post shader module creation failed");
    return nullptr;
  }

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

  VkPipelineDepthStencilStateCreateInfo depth{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};

  VkPipelineColorBlendAttachmentState blend_attachment{};
  blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 1;
  blend.pAttachments = &blend_attachment;

  VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;

  VkPipelineRenderingCreateInfo rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &output_format;

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

  VkResult result =
      vkCreateGraphicsPipelines(device.device(), VK_NULL_HANDLE, 1, &info, nullptr,
                                &pass->pipeline_);
  vkDestroyShaderModule(device.device(), vert, nullptr);
  vkDestroyShaderModule(device.device(), frag, nullptr);
  if (result != VK_SUCCESS) {
    REC_ERROR("post pipeline creation failed");
    return nullptr;
  }
  return pass;
}

PostPass::~PostPass() {
  if (pipeline_) vkDestroyPipeline(device_.device(), pipeline_, nullptr);
  if (layout_) vkDestroyPipelineLayout(device_.device(), layout_, nullptr);
  if (set_layout_) vkDestroyDescriptorSetLayout(device_.device(), set_layout_, nullptr);
  if (sampler_) vkDestroySampler(device_.device(), sampler_, nullptr);
}

void PostPass::Record(PassContext& ctx, VkImageView input, VkImageView output,
                      VkExtent2D output_extent, const Params& params) {
  VkDescriptorSet set = ctx.allocate_set(set_layout_);
  VkDescriptorImageInfo image_info{};
  image_info.sampler = sampler_;
  image_info.imageView = input;
  image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = set;
  write.dstBinding = 0;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.pImageInfo = &image_info;
  vkUpdateDescriptorSets(device_.device(), 1, &write, 0, nullptr);

  VkRenderingAttachmentInfo color{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color.imageView = output;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // fully overwritten
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

  VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea = {{0, 0}, output_extent};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &color;

  vkCmdBeginRendering(ctx.cmd, &rendering);
  VkViewport viewport{0, 0, static_cast<f32>(output_extent.width),
                      static_cast<f32>(output_extent.height), 0.0f, 1.0f};
  VkRect2D scissor{{0, 0}, output_extent};
  vkCmdSetViewport(ctx.cmd, 0, 1, &viewport);
  vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);
  vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
  vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0,
                          nullptr);
  vkCmdPushConstants(ctx.cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(params), &params);
  vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
  vkCmdEndRendering(ctx.cmd);
}

}  // namespace rec::render
