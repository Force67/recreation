#include "render/pipeline/mesh_pipeline.h"

#include "asset/mesh.h"
#include "core/log.h"
#include "render/util/shader_util.h"
#include "shaders/mesh_ps_hlsl.h"
#include "shaders/mesh_rt_ps_hlsl.h"
#include "shaders/mesh_skin_vs_hlsl.h"
#include "shaders/mesh_vs_hlsl.h"
#include "shaders/prepass_ps_hlsl.h"

namespace rec::render {

std::unique_ptr<MeshPipeline> MeshPipeline::Create(Device& device, VkFormat color_format,
                                                   VkFormat motion_format,
                                                   VkFormat normal_format, VkFormat depth_format,
                                                   VkDescriptorSetLayout material_layout,
                                                   VkDescriptorSetLayout environment_layout,
                                                   VkDescriptorSetLayout bindless_layout) {
  auto pipeline = std::unique_ptr<MeshPipeline>(new MeshPipeline(device));
  bool rt = device.caps().ray_query;
  pipeline->has_bindless_ = bindless_layout != VK_NULL_HANDLE;

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

  VkDescriptorSetLayout set_layouts[4] = {pipeline->set_layout_, material_layout,
                                          environment_layout, bindless_layout};
  VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = pipeline->has_bindless_ ? 4 : 3;
  layout_info.pSetLayouts = set_layouts;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push_range;
  if (vkCreatePipelineLayout(device.device(), &layout_info, nullptr, &pipeline->layout_) !=
      VK_SUCCESS) {
    return nullptr;
  }

  VkShaderModule vert = CreateShaderModule(device.device(), k_mesh_vs_hlsl, sizeof(k_mesh_vs_hlsl));
  VkShaderModule vert_skin =
      CreateShaderModule(device.device(), k_mesh_skin_vs_hlsl, sizeof(k_mesh_skin_vs_hlsl));
  VkShaderModule frag = CreateShaderModule(device.device(), k_mesh_ps_hlsl, sizeof(k_mesh_ps_hlsl));
  VkShaderModule frag_prepass =
      CreateShaderModule(device.device(), k_prepass_ps_hlsl, sizeof(k_prepass_ps_hlsl));
  VkShaderModule frag_rt =
      rt ? CreateShaderModule(device.device(), k_mesh_rt_ps_hlsl, sizeof(k_mesh_rt_ps_hlsl))
         : VK_NULL_HANDLE;
  if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE || frag_prepass == VK_NULL_HANDLE ||
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

  // Skinned variant: a second vertex stream (binding 1) carries 4 bone indices
  // and 4 normalized weights per vertex.
  VkVertexInputBindingDescription skinned_bindings[2] = {binding, {}};
  skinned_bindings[1].binding = 1;
  skinned_bindings[1].stride = sizeof(asset::SkinnedVertexExtra);
  skinned_bindings[1].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  VkVertexInputAttributeDescription skinned_attributes[7];
  for (int i = 0; i < 5; ++i) skinned_attributes[i] = attributes[i];
  skinned_attributes[5] = {.location = 5, .binding = 1, .format = VK_FORMAT_R8G8B8A8_UINT,
                           .offset = offsetof(asset::SkinnedVertexExtra, bone_indices)};
  skinned_attributes[6] = {.location = 6, .binding = 1, .format = VK_FORMAT_R8G8B8A8_UNORM,
                           .offset = offsetof(asset::SkinnedVertexExtra, bone_weights)};
  VkPipelineVertexInputStateCreateInfo skinned_vertex_input{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  skinned_vertex_input.vertexBindingDescriptionCount = 2;
  skinned_vertex_input.pVertexBindingDescriptions = skinned_bindings;
  skinned_vertex_input.vertexAttributeDescriptionCount = 7;
  skinned_vertex_input.pVertexAttributeDescriptions = skinned_attributes;

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

  // The prepass owns depth; main variants test EQUAL against it and leave
  // motion alone (the prepass already wrote it).
  VkPipelineDepthStencilStateCreateInfo depth{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable = VK_TRUE;
  depth.depthWriteEnable = VK_FALSE;
  depth.depthCompareOp = VK_COMPARE_OP_EQUAL;

  VkPipelineColorBlendAttachmentState blend_attachments[2]{};
  blend_attachments[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  blend_attachments[1].colorWriteMask = 0;  // motion comes from the prepass
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

  // Skinned main variants: same fragment shaders and main pass state, skinned
  // vertex stage + the bone weight stream. Wireframe is not skinned.
  if (vert_skin != VK_NULL_HANDLE) {
    stages[0].module = vert_skin;
    info.pVertexInputState = &skinned_vertex_input;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    for (u32 variant = 0; variant < 2; ++variant) {
      if ((variant & kRt) && !rt) continue;
      stages[1].module = (variant & kRt) ? frag_rt : frag;
      if (vkCreateGraphicsPipelines(device.device(), VK_NULL_HANDLE, 1, &info, nullptr,
                                    &pipeline->skinned_pipelines_[variant]) != VK_SUCCESS) {
        REC_ERROR("mesh skinned pipeline creation failed (variant {})", variant);
        pipeline->skinned_pipelines_[variant] = VK_NULL_HANDLE;
      }
    }
    stages[0].module = vert;
    info.pVertexInputState = &vertex_input;
  }

  // Transparent variants: blend over the opaque pass, test against its
  // depth without writing, shade with the same pbr shaders.
  depth.depthWriteEnable = VK_FALSE;
  depth.depthCompareOp = VK_COMPARE_OP_GREATER;  // reversed z
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  blend_attachments[0].blendEnable = VK_TRUE;
  blend_attachments[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  blend_attachments[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend_attachments[0].colorBlendOp = VK_BLEND_OP_ADD;
  blend_attachments[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blend_attachments[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  blend_attachments[0].alphaBlendOp = VK_BLEND_OP_ADD;
  for (u32 variant = 0; variant < 2; ++variant) {
    if (variant == 1 && !rt) continue;
    stages[1].module = variant == 1 ? frag_rt : frag;
    if (vkCreateGraphicsPipelines(device.device(), VK_NULL_HANDLE, 1, &info, nullptr,
                                  &pipeline->blend_pipelines_[variant]) != VK_SUCCESS) {
      REC_ERROR("mesh blend pipeline creation failed");
      pipeline->blend_pipelines_[variant] = VK_NULL_HANDLE;
    }
  }
  blend_attachments[0].blendEnable = VK_FALSE;

  // Prepass: depth write + normals/motion/depth-export targets, same layout.
  stages[1].module = frag_prepass;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  depth.depthWriteEnable = VK_TRUE;
  depth.depthCompareOp = VK_COMPARE_OP_GREATER;  // reversed z
  VkPipelineColorBlendAttachmentState prepass_blend[3]{};
  prepass_blend[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
  prepass_blend[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
  prepass_blend[2].colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
  blend.attachmentCount = 3;
  blend.pAttachments = prepass_blend;
  VkFormat prepass_formats[3] = {normal_format, motion_format, VK_FORMAT_R32_SFLOAT};
  rendering.colorAttachmentCount = 3;
  rendering.pColorAttachmentFormats = prepass_formats;
  if (vkCreateGraphicsPipelines(device.device(), VK_NULL_HANDLE, 1, &info, nullptr,
                                &pipeline->prepass_pipeline_) != VK_SUCCESS) {
    REC_ERROR("mesh prepass pipeline creation failed");
    pipeline->prepass_pipeline_ = VK_NULL_HANDLE;
  }

  // Skinned prepass: must match the skinned main pose so the EQUAL depth test
  // passes.
  if (vert_skin != VK_NULL_HANDLE) {
    stages[0].module = vert_skin;
    info.pVertexInputState = &skinned_vertex_input;
    if (vkCreateGraphicsPipelines(device.device(), VK_NULL_HANDLE, 1, &info, nullptr,
                                  &pipeline->skinned_prepass_pipeline_) != VK_SUCCESS) {
      REC_ERROR("mesh skinned prepass pipeline creation failed");
      pipeline->skinned_prepass_pipeline_ = VK_NULL_HANDLE;
    }
  }

  vkDestroyShaderModule(device.device(), vert, nullptr);
  if (vert_skin) vkDestroyShaderModule(device.device(), vert_skin, nullptr);
  vkDestroyShaderModule(device.device(), frag, nullptr);
  vkDestroyShaderModule(device.device(), frag_prepass, nullptr);
  if (frag_rt) vkDestroyShaderModule(device.device(), frag_rt, nullptr);
  if (pipeline->pipelines_[0] == VK_NULL_HANDLE || pipeline->prepass_pipeline_ == VK_NULL_HANDLE ||
      pipeline->blend_pipelines_[0] == VK_NULL_HANDLE) {
    return nullptr;
  }
  return pipeline;
}

MeshPipeline::~MeshPipeline() {
  for (VkPipeline pipeline : pipelines_) {
    if (pipeline) vkDestroyPipeline(device_.device(), pipeline, nullptr);
  }
  for (VkPipeline pipeline : blend_pipelines_) {
    if (pipeline) vkDestroyPipeline(device_.device(), pipeline, nullptr);
  }
  for (VkPipeline pipeline : skinned_pipelines_) {
    if (pipeline) vkDestroyPipeline(device_.device(), pipeline, nullptr);
  }
  if (skinned_prepass_pipeline_) {
    vkDestroyPipeline(device_.device(), skinned_prepass_pipeline_, nullptr);
  }
  if (prepass_pipeline_) vkDestroyPipeline(device_.device(), prepass_pipeline_, nullptr);
  if (layout_) vkDestroyPipelineLayout(device_.device(), layout_, nullptr);
  if (set_layout_) vkDestroyDescriptorSetLayout(device_.device(), set_layout_, nullptr);
}

void MeshPipeline::Bind(VkCommandBuffer cmd, VkDescriptorSet globals,
                        VkDescriptorSet environment, VkDescriptorSet bindless, bool use_rt,
                        bool wireframe) {
  u32 variant = (use_rt ? kRt : 0) | (wireframe ? kWire : 0);
  if (pipelines_[variant] == VK_NULL_HANDLE) variant &= ~kWire;
  if (pipelines_[variant] == VK_NULL_HANDLE) variant = 0;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_[variant]);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &globals, 0,
                          nullptr);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 2, 1, &environment, 0,
                          nullptr);
  if (has_bindless_ && bindless != VK_NULL_HANDLE) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 3, 1, &bindless, 0,
                            nullptr);
  }
}

void MeshPipeline::BindBlend(VkCommandBuffer cmd, VkDescriptorSet globals,
                             VkDescriptorSet environment, VkDescriptorSet bindless, bool use_rt) {
  u32 variant = use_rt && blend_pipelines_[1] != VK_NULL_HANDLE ? 1 : 0;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blend_pipelines_[variant]);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &globals, 0,
                          nullptr);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 2, 1, &environment, 0,
                          nullptr);
  if (has_bindless_ && bindless != VK_NULL_HANDLE) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 3, 1, &bindless, 0,
                            nullptr);
  }
}

void MeshPipeline::BindPrepass(VkCommandBuffer cmd, VkDescriptorSet globals) {
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, prepass_pipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &globals, 0,
                          nullptr);
}

void MeshPipeline::BindMaterial(VkCommandBuffer cmd, VkDescriptorSet material) {
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 1, 1, &material, 0,
                          nullptr);
}

void MeshPipeline::SetSkinned(VkCommandBuffer cmd, bool skinned, bool use_rt, bool wireframe) {
  if (skinned) {
    VkPipeline p = skinned_pipelines_[use_rt ? kRt : 0];
    if (p == VK_NULL_HANDLE) p = skinned_pipelines_[0];
    if (p != VK_NULL_HANDLE) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    return;
  }
  u32 variant = (use_rt ? kRt : 0) | (wireframe ? kWire : 0);
  if (pipelines_[variant] == VK_NULL_HANDLE) variant &= ~kWire;
  if (pipelines_[variant] == VK_NULL_HANDLE) variant = 0;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_[variant]);
}

void MeshPipeline::SetPrepassSkinned(VkCommandBuffer cmd, bool skinned) {
  VkPipeline p = skinned && skinned_prepass_pipeline_ ? skinned_prepass_pipeline_ : prepass_pipeline_;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
}

void MeshPipeline::Draw(VkCommandBuffer cmd, const GpuMesh& mesh, const MeshPushConstants& push) {
  vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
  VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertices.buffer, &offset);
  if (mesh.skinned && mesh.skinning.buffer != VK_NULL_HANDLE) {
    vkCmdBindVertexBuffers(cmd, 1, 1, &mesh.skinning.buffer, &offset);
  }
  vkCmdBindIndexBuffer(cmd, mesh.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
}

void MeshPipeline::DrawSubmesh(VkCommandBuffer cmd, const GpuSubmesh& submesh) {
  vkCmdDrawIndexed(cmd, submesh.index_count, 1, submesh.index_offset, 0, 0);
}

}  // namespace rec::render
