#include "render/mesh_pipeline.h"

#include "asset/mesh.h"
#include "core/log.h"
#include "render/shader_util.h"
#include "shaders/mesh_ps_hlsl.h"
#include "shaders/mesh_rt_ps_hlsl.h"
#include "shaders/mesh_vs_hlsl.h"

namespace rec::render {

std::unique_ptr<MeshPipeline> MeshPipeline::Create(Device& device, VkFormat color_format,
                                                   VkFormat motion_format, VkFormat depth_format,
                                                   VkDescriptorSetLayout material_layout) {
  auto pipeline = std::unique_ptr<MeshPipeline>(new MeshPipeline(device));
  bool rt = device.caps().ray_query;

  VkDescriptorSetLayoutBinding bindings[2]{};
  bindings[0].binding = 0;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  bindings[1].binding = 1;
  bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
  bindings[1].descriptorCount = 1;
  bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutCreateInfo set_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = rt ? 2 : 1;
  set_info.pBindings = bindings;
  if (vkCreateDescriptorSetLayout(device.device(), &set_info, nullptr, &pipeline->set_layout_) !=
      VK_SUCCESS) {
    return nullptr;
  }

  VkPushConstantRange push_range{};
  push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  push_range.size = sizeof(MeshPushConstants);

  VkDescriptorSetLayout set_layouts[2] = {pipeline->set_layout_, material_layout};
  VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 2;
  layout_info.pSetLayouts = set_layouts;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push_range;
  if (vkCreatePipelineLayout(device.device(), &layout_info, nullptr, &pipeline->layout_) !=
      VK_SUCCESS) {
    return nullptr;
  }

  VkShaderModule vert = CreateShaderModule(device.device(), k_mesh_vs_hlsl, sizeof(k_mesh_vs_hlsl));
  VkShaderModule frag = CreateShaderModule(device.device(), k_mesh_ps_hlsl, sizeof(k_mesh_ps_hlsl));
  VkShaderModule frag_rt =
      rt ? CreateShaderModule(device.device(), k_mesh_rt_ps_hlsl, sizeof(k_mesh_rt_ps_hlsl))
         : VK_NULL_HANDLE;
  if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE ||
      (rt && frag_rt == VK_NULL_HANDLE)) {
    REC_ERROR("mesh shader module creation failed");
    return nullptr;
  }

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
  // TODO: back face culling once converted content settles winding order.
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

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
  VkPipelineRenderingCreateInfo rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 2;
  rendering.pColorAttachmentFormats = color_formats;
  rendering.depthAttachmentFormat = depth_format;

  VkPipelineShaderStageCreateInfo stages[2];
  stages[0] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].pName = "main";

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
  info.layout = pipeline->layout_;

  bool wire_capable = device.caps().fill_mode_non_solid;
  for (u32 variant = 0; variant < 4; ++variant) {
    if ((variant & kRt) && !rt) continue;
    if ((variant & kWire) && !wire_capable) continue;
    stages[1].module = (variant & kRt) ? frag_rt : frag;
    raster.polygonMode = (variant & kWire) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    if (vkCreateGraphicsPipelines(device.device(), VK_NULL_HANDLE, 1, &info, nullptr,
                                  &pipeline->pipelines_[variant]) != VK_SUCCESS) {
      REC_ERROR("mesh pipeline creation failed (variant {})", variant);
      pipeline->pipelines_[variant] = VK_NULL_HANDLE;
    }
  }

  vkDestroyShaderModule(device.device(), vert, nullptr);
  vkDestroyShaderModule(device.device(), frag, nullptr);
  if (frag_rt) vkDestroyShaderModule(device.device(), frag_rt, nullptr);
  if (pipeline->pipelines_[0] == VK_NULL_HANDLE) return nullptr;
  return pipeline;
}

MeshPipeline::~MeshPipeline() {
  for (VkPipeline pipeline : pipelines_) {
    if (pipeline) vkDestroyPipeline(device_.device(), pipeline, nullptr);
  }
  if (layout_) vkDestroyPipelineLayout(device_.device(), layout_, nullptr);
  if (set_layout_) vkDestroyDescriptorSetLayout(device_.device(), set_layout_, nullptr);
}

void MeshPipeline::Bind(VkCommandBuffer cmd, VkDescriptorSet globals, bool rt_shadows,
                        bool wireframe) {
  u32 variant = (rt_shadows ? kRt : 0) | (wireframe ? kWire : 0);
  if (pipelines_[variant] == VK_NULL_HANDLE) variant &= ~kWire;
  if (pipelines_[variant] == VK_NULL_HANDLE) variant = 0;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_[variant]);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &globals, 0,
                          nullptr);
}

void MeshPipeline::BindMaterial(VkCommandBuffer cmd, VkDescriptorSet material) {
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 1, 1, &material, 0,
                          nullptr);
}

void MeshPipeline::Draw(VkCommandBuffer cmd, const GpuMesh& mesh, const MeshPushConstants& push) {
  vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
  VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertices.buffer, &offset);
  vkCmdBindIndexBuffer(cmd, mesh.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
}

void MeshPipeline::DrawSubmesh(VkCommandBuffer cmd, const GpuSubmesh& submesh) {
  vkCmdDrawIndexed(cmd, submesh.index_count, 1, submesh.index_offset, 0, 0);
}

}  // namespace rec::render
