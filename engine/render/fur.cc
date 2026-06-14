#include "render/fur.h"

#include "asset/primitives.h"
#include "core/log.h"
#include "render/rhi/device.h"
#include "render/shader_util.h"
#include "shaders/fur_ps_hlsl.h"
#include "shaders/fur_vs_hlsl.h"

namespace rec::render {
namespace {

struct FurPush {
  Mat4 view_proj;
  Mat4 model;
  f32 sun_dir[3];
  f32 fur_length;
  f32 sun_color[3];
  u32 shell_count;
  f32 base_color[3];
  f32 ambient;
};

}  // namespace

bool FurPass::Initialize(Device& device, VkFormat color_format, VkFormat depth_format) {
  asset::Mesh sphere = asset::MakeSphere(radius_, 64, 96, asset::MakeAssetId("builtin/fur/sphere"));
  const asset::MeshLod& lod = sphere.lods[0];
  index_count_ = static_cast<u32>(lod.indices.size());
  vertices_ = device.CreateBufferWithData(
      ByteSpan(reinterpret_cast<const u8*>(lod.vertices.data()),
               lod.vertices.size() * sizeof(asset::Vertex)),
      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  indices_ = device.CreateBufferWithData(
      ByteSpan(reinterpret_cast<const u8*>(lod.indices.data()), lod.indices.size() * sizeof(u32)),
      VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

  VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(FurPush)};
  VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push;
  if (vkCreatePipelineLayout(device.device(), &layout_info, nullptr, &layout_) != VK_SUCCESS) {
    return false;
  }

  VkShaderModule vs = CreateShaderModule(device.device(), k_fur_vs_hlsl, sizeof(k_fur_vs_hlsl));
  VkShaderModule ps = CreateShaderModule(device.device(), k_fur_ps_hlsl, sizeof(k_fur_ps_hlsl));
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

  VkVertexInputBindingDescription binding{};
  binding.stride = sizeof(asset::Vertex);
  binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  VkVertexInputAttributeDescription attrs[3] = {
      {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,
       .offset = offsetof(asset::Vertex, position)},
      {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,
       .offset = offsetof(asset::Vertex, normal)},
      {.location = 3, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,
       .offset = offsetof(asset::Vertex, uv)}};
  VkPipelineVertexInputStateCreateInfo vertex_input{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex_input.vertexBindingDescriptionCount = 1;
  vertex_input.pVertexBindingDescriptions = &binding;
  vertex_input.vertexAttributeDescriptionCount = 3;
  vertex_input.pVertexAttributeDescriptions = attrs;

  VkPipelineInputAssemblyStateCreateInfo ia{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo viewport{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_BACK_BIT;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo ms{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo ds{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = VK_TRUE;
  ds.depthWriteEnable = VK_FALSE;  // shells alpha-blend; the core owns the depth
  ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;  // reversed z

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
  rendering.depthAttachmentFormat = depth_format;

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
    REC_ERROR("fur pipeline creation failed");
    return false;
  }
  return true;
}

void FurPass::AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                         const Mat4& model, const Mat4& view_proj, const Vec3& sun_dir,
                         const Vec3& sun_color, f32 ambient, const Params& params) {
  graph.AddPass(
      "fur",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Write(color, ResourceUsage::kColorAttachment);
        builder.Write(depth, ResourceUsage::kDepthAttachment);
      },
      [this, color, depth, model, view_proj, sun_dir, sun_color, ambient, params](PassContext& ctx) {
        const GpuImage& target = ctx.graph->image(color);
        VkRenderingAttachmentInfo col{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        col.imageView = target.view;
        col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        col.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingAttachmentInfo dep{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        dep.imageView = ctx.graph->image(depth).view;
        dep.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        dep.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        dep.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, target.extent};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &col;
        rendering.pDepthAttachment = &dep;
        vkCmdBeginRendering(ctx.cmd, &rendering);

        VkViewport vp{0, 0, static_cast<f32>(target.extent.width),
                      static_cast<f32>(target.extent.height), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, target.extent};
        vkCmdSetViewport(ctx.cmd, 0, 1, &vp);
        vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

        FurPush push{};
        push.view_proj = view_proj;
        push.model = model;
        push.sun_dir[0] = sun_dir.x;
        push.sun_dir[1] = sun_dir.y;
        push.sun_dir[2] = sun_dir.z;
        push.fur_length = params.fur_length;
        push.sun_color[0] = sun_color.x;
        push.sun_color[1] = sun_color.y;
        push.sun_color[2] = sun_color.z;
        push.shell_count = params.shell_count;
        push.base_color[0] = params.base_color[0];
        push.base_color[1] = params.base_color[1];
        push.base_color[2] = params.base_color[2];
        push.ambient = ambient;
        vkCmdPushConstants(ctx.cmd, layout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &vertices_.buffer, &offset);
        vkCmdBindIndexBuffer(ctx.cmd, indices_.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(ctx.cmd, index_count_, params.shell_count, 0, 0, 0);
        vkCmdEndRendering(ctx.cmd);
      });
}

void FurPass::Destroy(Device& device) {
  if (pipeline_) vkDestroyPipeline(device.device(), pipeline_, nullptr);
  if (layout_) vkDestroyPipelineLayout(device.device(), layout_, nullptr);
  device.DestroyBuffer(vertices_);
  device.DestroyBuffer(indices_);
  pipeline_ = VK_NULL_HANDLE;
  layout_ = VK_NULL_HANDLE;
}

}  // namespace rec::render
