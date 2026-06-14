#include "render/gaussian.h"

#include <algorithm>
#include <cstring>

#include "core/log.h"
#include "render/shader_util.h"
#include "shaders/gsplat_ps_hlsl.h"
#include "shaders/gsplat_vs_hlsl.h"

namespace rec::render {
namespace {

struct GaussianPush {
  Mat4 view;
  f32 proj_x;
  f32 proj_y;
  f32 near_plane;
  f32 screen_x;
  f32 screen_y;
  f32 pad[3];
};

}  // namespace

bool GaussianSplat::Initialize(Device& device, VkFormat color_format) {
  VkDescriptorSetLayoutBinding binding{};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  VkDescriptorSetLayoutCreateInfo set_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 1;
  set_info.pBindings = &binding;
  if (vkCreateDescriptorSetLayout(device.device(), &set_info, nullptr, &set_layout_) != VK_SUCCESS) {
    return false;
  }

  VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GaussianPush)};
  VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push;
  if (vkCreatePipelineLayout(device.device(), &layout_info, nullptr, &layout_) != VK_SUCCESS) {
    return false;
  }

  VkShaderModule vs = CreateShaderModule(device.device(), k_gsplat_vs_hlsl, sizeof(k_gsplat_vs_hlsl));
  VkShaderModule ps = CreateShaderModule(device.device(), k_gsplat_ps_hlsl, sizeof(k_gsplat_ps_hlsl));
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
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};

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
    REC_ERROR("gaussian pipeline creation failed");
    return false;
  }

  for (u32 i = 0; i < kFramesInFlight; ++i) {
    buffers_[i] = device.CreateBuffer(static_cast<u64>(kMaxGaussians) * sizeof(GaussianInstance),
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    if (!buffers_[i].mapped) return false;
  }
  return true;
}

void GaussianSplat::AddToGraph(RenderGraph& graph, ResourceHandle color,
                               const base::Vector<GaussianInstance>& gaussians, const Frame& frame,
                               u32 frame_slot) {
  if (gaussians.empty()) return;
  u32 count = std::min(static_cast<u32>(gaussians.size()), kMaxGaussians);

  // Sort back-to-front by view depth (front = -z, so most-negative first).
  base::Vector<u32> order(count);
  for (u32 i = 0; i < count; ++i) order[i] = i;
  const Mat4& v = frame.view;
  auto view_z = [&](u32 i) {
    const GaussianInstance& g = gaussians[i];
    return v.m[2] * g.position[0] + v.m[6] * g.position[1] + v.m[10] * g.position[2] + v.m[14];
  };
  std::sort(order.begin(), order.end(), [&](u32 a, u32 b) { return view_z(a) < view_z(b); });

  GaussianInstance* dst = static_cast<GaussianInstance*>(buffers_[frame_slot].mapped);
  for (u32 i = 0; i < count; ++i) dst[i] = gaussians[order[i]];
  VkBuffer buffer = buffers_[frame_slot].buffer;

  graph.AddPass(
      "gaussian_splat",
      [&](RenderGraph::PassBuilder& builder) { builder.Write(color, ResourceUsage::kColorAttachment); },
      [this, color, buffer, count, frame](PassContext& ctx) {
        VkDescriptorSet set = ctx.allocate_set(set_layout_);
        VkDescriptorBufferInfo buffer_info{buffer, 0, count * sizeof(GaussianInstance)};
        VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &buffer_info;
        vkUpdateDescriptorSets(ctx.device->device(), 1, &write, 0, nullptr);

        const GpuImage& target = ctx.graph->image(color);
        VkRenderingAttachmentInfo attachment{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView = target.view;
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
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

        GaussianPush push{};
        push.view = frame.view;
        push.proj_x = frame.proj_x;
        push.proj_y = frame.proj_y;
        push.near_plane = frame.near_plane;
        push.screen_x = frame.screen_x;
        push.screen_y = frame.screen_y;
        vkCmdPushConstants(ctx.cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        vkCmdDraw(ctx.cmd, 4, count, 0, 0);
        vkCmdEndRendering(ctx.cmd);
      });
}

void GaussianSplat::Destroy(Device& device) {
  if (pipeline_) vkDestroyPipeline(device.device(), pipeline_, nullptr);
  if (layout_) vkDestroyPipelineLayout(device.device(), layout_, nullptr);
  if (set_layout_) vkDestroyDescriptorSetLayout(device.device(), set_layout_, nullptr);
  for (u32 i = 0; i < kFramesInFlight; ++i) device.DestroyBuffer(buffers_[i]);
}

}  // namespace rec::render
