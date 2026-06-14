#include "render/particles.h"

#include <algorithm>
#include <cstring>

#include "core/log.h"
#include "render/shader_util.h"
#include "shaders/particle_ps_hlsl.h"
#include "shaders/particle_vs_hlsl.h"

namespace rec::render {
namespace {

struct ParticlePush {
  Mat4 view_proj;
  f32 cam_right[3];
  f32 near_plane;
  f32 cam_up[3];
  f32 soft_fade;
  f32 sun_dir[3];
  f32 sun_intensity;
  f32 sun_color[3];
  f32 ambient;
};

}  // namespace

bool ParticleSystem::Initialize(Device& device, VkFormat color_format) {
  device_ = &device;

  VkDescriptorSetLayoutBinding bindings[2]{};
  bindings[0].binding = 0;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  bindings[1].binding = 1;
  bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  bindings[1].descriptorCount = 1;
  bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutCreateInfo set_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 2;
  set_info.pBindings = bindings;
  if (vkCreateDescriptorSetLayout(device.device(), &set_info, nullptr, &set_layout_) !=
      VK_SUCCESS) {
    return false;
  }

  VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(ParticlePush)};
  VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push;
  if (vkCreatePipelineLayout(device.device(), &layout_info, nullptr, &layout_) != VK_SUCCESS) {
    return false;
  }

  VkShaderModule vs =
      CreateShaderModule(device.device(), k_particle_vs_hlsl, sizeof(k_particle_vs_hlsl));
  VkShaderModule ps =
      CreateShaderModule(device.device(), k_particle_ps_hlsl, sizeof(k_particle_ps_hlsl));
  if (vs == VK_NULL_HANDLE || ps == VK_NULL_HANDLE) return false;
  VkPipelineShaderStageCreateInfo stages[2];
  stages[0] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs;
  stages[0].pName = "main";
  stages[1] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = ps;
  stages[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vertex_input{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  VkPipelineViewportStateCreateInfo viewport{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo ds{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};  // no test/write

  VkPipelineColorBlendAttachmentState blend{};
  blend.blendEnable = VK_TRUE;
  blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.colorBlendOp = VK_BLEND_OP_ADD;
  blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blend.alphaBlendOp = VK_BLEND_OP_ADD;
  blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend_state{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend_state.attachmentCount = 1;
  blend_state.pAttachments = &blend;

  VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamics;

  VkPipelineRenderingCreateInfo rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &color_format;

  VkGraphicsPipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext = &rendering;
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &ia;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &ms;
  info.pDepthStencilState = &ds;
  info.pColorBlendState = &blend_state;
  info.pDynamicState = &dynamic;
  info.layout = layout_;
  VkResult r =
      vkCreateGraphicsPipelines(device.device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_);
  vkDestroyShaderModule(device.device(), vs, nullptr);
  vkDestroyShaderModule(device.device(), ps, nullptr);
  if (r != VK_SUCCESS) {
    REC_ERROR("particle pipeline creation failed");
    return false;
  }

  for (u32 i = 0; i < kFramesInFlight; ++i) {
    buffers_[i] = device.CreateBuffer(static_cast<u64>(kMaxParticles) * sizeof(ParticleInstance),
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    if (!buffers_[i].mapped) return false;
  }
  return true;
}

void ParticleSystem::AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                                const base::Vector<ParticleInstance>& particles, const Frame& frame,
                                u32 frame_slot) {
  if (particles.empty()) return;
  u32 count = std::min(static_cast<u32>(particles.size()), kMaxParticles);
  std::memcpy(buffers_[frame_slot].mapped, particles.data(), count * sizeof(ParticleInstance));
  VkBuffer buffer = buffers_[frame_slot].buffer;

  graph.AddPass(
      "particles",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Write(color, ResourceUsage::kColorAttachment);
        builder.Read(depth, ResourceUsage::kSampledFragment);
      },
      [this, color, depth, buffer, count, frame](PassContext& ctx) {
        VkDescriptorSet set = ctx.allocate_set(set_layout_);
        VkDescriptorBufferInfo buffer_info{buffer, 0, count * sizeof(ParticleInstance)};
        VkDescriptorImageInfo depth_info{VK_NULL_HANDLE, ctx.graph->image(depth).view,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet writes[2];
        writes[0] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo = &buffer_info;
        writes[1] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[1].dstSet = set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[1].pImageInfo = &depth_info;
        vkUpdateDescriptorSets(ctx.device->device(), 2, writes, 0, nullptr);

        const GpuImage& target = ctx.graph->image(color);
        VkRenderingAttachmentInfo attachment{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView = target.view;
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;  // blend over the lit scene
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, target.extent};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &attachment;
        vkCmdBeginRendering(ctx.cmd, &rendering);

        VkViewport vp{0, 0, static_cast<f32>(target.extent.width),
                      static_cast<f32>(target.extent.height), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, target.extent};
        vkCmdSetViewport(ctx.cmd, 0, 1, &vp);
        vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0,
                                nullptr);

        ParticlePush push{};
        push.view_proj = frame.view_proj;
        push.cam_right[0] = frame.cam_right.x;
        push.cam_right[1] = frame.cam_right.y;
        push.cam_right[2] = frame.cam_right.z;
        push.near_plane = frame.near_plane;
        push.cam_up[0] = frame.cam_up.x;
        push.cam_up[1] = frame.cam_up.y;
        push.cam_up[2] = frame.cam_up.z;
        push.soft_fade = frame.soft_fade;
        push.sun_dir[0] = frame.sun_direction.x;
        push.sun_dir[1] = frame.sun_direction.y;
        push.sun_dir[2] = frame.sun_direction.z;
        push.sun_intensity = frame.sun_intensity;
        push.sun_color[0] = frame.sun_color.x;
        push.sun_color[1] = frame.sun_color.y;
        push.sun_color[2] = frame.sun_color.z;
        push.ambient = frame.ambient;
        vkCmdPushConstants(ctx.cmd, layout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        vkCmdDraw(ctx.cmd, 4, count, 0, 0);
        vkCmdEndRendering(ctx.cmd);
      });
}

void ParticleSystem::Destroy(Device& device) {
  if (pipeline_) vkDestroyPipeline(device.device(), pipeline_, nullptr);
  if (layout_) vkDestroyPipelineLayout(device.device(), layout_, nullptr);
  if (set_layout_) vkDestroyDescriptorSetLayout(device.device(), set_layout_, nullptr);
  for (u32 i = 0; i < kFramesInFlight; ++i) device.DestroyBuffer(buffers_[i]);
}

}  // namespace rec::render
