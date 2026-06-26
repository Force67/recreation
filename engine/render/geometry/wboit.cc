#include "render/geometry/wboit.h"

#include "asset/primitives.h"
#include "core/log.h"
#include "render/rhi/device.h"
#include "render/util/shader_util.h"
#include "shaders/fullscreen_vs_hlsl.h"
#include "shaders/wboit_ps_hlsl.h"
#include "shaders/wboit_resolve_ps_hlsl.h"
#include "shaders/wboit_vs_hlsl.h"

namespace rec::render {
namespace {

constexpr VkFormat kAccumFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kRevealFormat = VK_FORMAT_R16_SFLOAT;

struct WboitPush {
  Mat4 view_proj;
  Mat4 model;
  f32 color[4];
  f32 sun_dir[3];
  f32 pad0;
  f32 sun_color[3];
  f32 ambient;
};

}  // namespace

bool WboitPass::Initialize(Device& device, VkFormat color_format, VkFormat depth_format) {
  color_format_ = color_format;
  asset::Mesh sphere = asset::MakeSphere(1.0f, 40, 60, asset::MakeAssetId("builtin/oit/sphere"));
  const asset::MeshLod& lod = sphere.lods[0];
  index_count_ = static_cast<u32>(lod.indices.size());
  vertices_ = device.CreateBufferWithData(
      ByteSpan(reinterpret_cast<const u8*>(lod.vertices.data()),
               lod.vertices.size() * sizeof(asset::Vertex)),
      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  indices_ = device.CreateBufferWithData(
      ByteSpan(reinterpret_cast<const u8*>(lod.indices.data()), lod.indices.size() * sizeof(u32)),
      VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

  // --- Geometry pipeline: accumulate into the two oit targets. ---
  VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(WboitPush)};
  VkPipelineLayoutCreateInfo gl{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  gl.pushConstantRangeCount = 1;
  gl.pPushConstantRanges = &push;
  if (vkCreatePipelineLayout(device.device(), &gl, nullptr, &geom_layout_) != VK_SUCCESS) {
    return false;
  }

  VkShaderModule vs = CreateShaderModule(device.device(), k_wboit_vs_hlsl, sizeof(k_wboit_vs_hlsl));
  VkShaderModule ps = CreateShaderModule(device.device(), k_wboit_ps_hlsl, sizeof(k_wboit_ps_hlsl));
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
  VkVertexInputAttributeDescription attrs[2] = {
      {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,
       .offset = offsetof(asset::Vertex, position)},
      {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,
       .offset = offsetof(asset::Vertex, normal)}};
  VkPipelineVertexInputStateCreateInfo vi{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = 1;
  vi.pVertexBindingDescriptions = &binding;
  vi.vertexAttributeDescriptionCount = 2;
  vi.pVertexAttributeDescriptions = attrs;

  VkPipelineInputAssemblyStateCreateInfo ia{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;  // see through both faces
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo ds{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = VK_TRUE;
  ds.depthWriteEnable = VK_FALSE;
  ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;  // reversed z, occluded by opaque

  // accum: additive. revealage: dst *= (1 - src.r).
  VkPipelineColorBlendAttachmentState blends[2]{};
  blends[0].blendEnable = VK_TRUE;
  blends[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  blends[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
  blends[0].colorBlendOp = VK_BLEND_OP_ADD;
  blends[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blends[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blends[0].alphaBlendOp = VK_BLEND_OP_ADD;
  blends[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  blends[1].blendEnable = VK_TRUE;
  blends[1].srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
  blends[1].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
  blends[1].colorBlendOp = VK_BLEND_OP_ADD;
  blends[1].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  blends[1].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blends[1].alphaBlendOp = VK_BLEND_OP_ADD;
  blends[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
  VkPipelineColorBlendStateCreateInfo blend{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 2;
  blend.pAttachments = blends;

  VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dyn;
  VkFormat color_formats[2] = {kAccumFormat, kRevealFormat};
  VkPipelineRenderingCreateInfo rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 2;
  rendering.pColorAttachmentFormats = color_formats;
  rendering.depthAttachmentFormat = depth_format;

  VkGraphicsPipelineCreateInfo gi{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gi.pNext = &rendering;
  gi.stageCount = 2;
  gi.pStages = stages;
  gi.pVertexInputState = &vi;
  gi.pInputAssemblyState = &ia;
  gi.pViewportState = &vp;
  gi.pRasterizationState = &rs;
  gi.pMultisampleState = &ms;
  gi.pDepthStencilState = &ds;
  gi.pColorBlendState = &blend;
  gi.pDynamicState = &dynamic;
  gi.layout = geom_layout_;
  VkResult r =
      vkCreateGraphicsPipelines(device.device(), VK_NULL_HANDLE, 1, &gi, nullptr, &geom_pipeline_);
  vkDestroyShaderModule(device.device(), vs, nullptr);
  vkDestroyShaderModule(device.device(), ps, nullptr);
  if (r != VK_SUCCESS) {
    REC_ERROR("wboit geometry pipeline creation failed");
    return false;
  }

  // --- Resolve pipeline: composite the oit targets over the scene. ---
  VkSamplerCreateInfo si{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = si.minFilter = VK_FILTER_NEAREST;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  vkCreateSampler(device.device(), &si, nullptr, &sampler_);

  VkDescriptorSetLayoutBinding rb[3]{};
  for (u32 i = 0; i < 3; ++i) {
    rb[i].binding = i;
    rb[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    rb[i].descriptorCount = 1;
    rb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo rsi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  rsi.bindingCount = 3;
  rsi.pBindings = rb;
  if (vkCreateDescriptorSetLayout(device.device(), &rsi, nullptr, &resolve_set_layout_) !=
      VK_SUCCESS) {
    return false;
  }
  VkPipelineLayoutCreateInfo rl{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  rl.setLayoutCount = 1;
  rl.pSetLayouts = &resolve_set_layout_;
  if (vkCreatePipelineLayout(device.device(), &rl, nullptr, &resolve_layout_) != VK_SUCCESS) {
    return false;
  }
  VkShaderModule fvs =
      CreateShaderModule(device.device(), k_fullscreen_vs_hlsl, sizeof(k_fullscreen_vs_hlsl));
  VkShaderModule rps =
      CreateShaderModule(device.device(), k_wboit_resolve_ps_hlsl, sizeof(k_wboit_resolve_ps_hlsl));
  if (fvs == VK_NULL_HANDLE || rps == VK_NULL_HANDLE) return false;
  VkPipelineShaderStageCreateInfo rstages[2];
  rstages[0] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  rstages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  rstages[0].module = fvs;
  rstages[0].pName = "main";
  rstages[1] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  rstages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  rstages[1].module = rps;
  rstages[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo rvi{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineDepthStencilStateCreateInfo rds{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  VkPipelineColorBlendAttachmentState rblend{};
  rblend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo rbs{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  rbs.attachmentCount = 1;
  rbs.pAttachments = &rblend;
  VkPipelineRenderingCreateInfo rrendering{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rrendering.colorAttachmentCount = 1;
  rrendering.pColorAttachmentFormats = &color_format_;
  VkGraphicsPipelineCreateInfo ri{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  ri.pNext = &rrendering;
  ri.stageCount = 2;
  ri.pStages = rstages;
  ri.pVertexInputState = &rvi;
  ri.pInputAssemblyState = &ia;
  ri.pViewportState = &vp;
  ri.pRasterizationState = &rs;
  ri.pMultisampleState = &ms;
  ri.pDepthStencilState = &rds;
  ri.pColorBlendState = &rbs;
  ri.pDynamicState = &dynamic;
  ri.layout = resolve_layout_;
  VkResult rr =
      vkCreateGraphicsPipelines(device.device(), VK_NULL_HANDLE, 1, &ri, nullptr, &resolve_pipeline_);
  vkDestroyShaderModule(device.device(), fvs, nullptr);
  vkDestroyShaderModule(device.device(), rps, nullptr);
  if (rr != VK_SUCCESS) {
    REC_ERROR("wboit resolve pipeline creation failed");
    return false;
  }
  return true;
}

ResourceHandle WboitPass::AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                                     const base::Vector<WboitInstance>& instances,
                                     const Mat4& view_proj, const Vec3& sun_dir,
                                     const Vec3& sun_color, f32 ambient, u32 width, u32 height) {
  ResourceHandle accum =
      graph.CreateTexture({.name = "oit_accum", .format = kAccumFormat, .width = width, .height = height});
  ResourceHandle reveal =
      graph.CreateTexture({.name = "oit_reveal", .format = kRevealFormat, .width = width, .height = height});
  ResourceHandle composite = graph.CreateTexture(
      {.name = "oit_composite", .format = color_format_, .width = width, .height = height});

  WboitPush base{};
  base.view_proj = view_proj;
  base.sun_dir[0] = sun_dir.x;
  base.sun_dir[1] = sun_dir.y;
  base.sun_dir[2] = sun_dir.z;
  base.sun_color[0] = sun_color.x;
  base.sun_color[1] = sun_color.y;
  base.sun_color[2] = sun_color.z;
  base.ambient = ambient;

  graph.AddPass(
      "oit_accumulate",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Write(accum, ResourceUsage::kColorAttachment);
        builder.Write(reveal, ResourceUsage::kColorAttachment);
        builder.Write(depth, ResourceUsage::kDepthAttachment);
      },
      [this, accum, reveal, depth, instances, base, width, height](PassContext& ctx) {
        VkRenderingAttachmentInfo colors[2];
        colors[0] = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colors[0].imageView = ctx.graph->image(accum).view;
        colors[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colors[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colors[0].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        colors[1] = colors[0];
        colors[1].imageView = ctx.graph->image(reveal).view;
        colors[1].clearValue.color = {{1.0f, 0.0f, 0.0f, 0.0f}};  // full transmittance
        VkRenderingAttachmentInfo depth_att{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth_att.imageView = ctx.graph->image(depth).view;
        depth_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, {width, height}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 2;
        rendering.pColorAttachments = colors;
        rendering.pDepthAttachment = &depth_att;
        vkCmdBeginRendering(ctx.cmd, &rendering);
        VkViewport vp{0, 0, static_cast<f32>(width), static_cast<f32>(height), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {width, height}};
        vkCmdSetViewport(ctx.cmd, 0, 1, &vp);
        vkCmdSetScissor(ctx.cmd, 0, 1, &sc);
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, geom_pipeline_);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &vertices_.buffer, &offset);
        vkCmdBindIndexBuffer(ctx.cmd, indices_.buffer, 0, VK_INDEX_TYPE_UINT32);
        for (const WboitInstance& inst : instances) {
          WboitPush push = base;
          push.model = inst.model;
          push.color[0] = inst.color[0];
          push.color[1] = inst.color[1];
          push.color[2] = inst.color[2];
          push.color[3] = inst.color[3];
          vkCmdPushConstants(ctx.cmd, geom_layout_,
                             VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                             sizeof(push), &push);
          vkCmdDrawIndexed(ctx.cmd, index_count_, 1, 0, 0, 0);
        }
        vkCmdEndRendering(ctx.cmd);
      });

  graph.AddPass(
      "oit_resolve",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(accum, ResourceUsage::kSampledFragment);
        builder.Read(reveal, ResourceUsage::kSampledFragment);
        builder.Read(color, ResourceUsage::kSampledFragment);
        builder.Write(composite, ResourceUsage::kColorAttachment);
      },
      [this, accum, reveal, color, composite, width, height](PassContext& ctx) {
        VkDescriptorSet set = ctx.allocate_set(resolve_set_layout_);
        VkDescriptorImageInfo imgs[3] = {
            {sampler_, ctx.graph->image(accum).view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {sampler_, ctx.graph->image(reveal).view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {sampler_, ctx.graph->image(color).view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet w[3];
        for (u32 i = 0; i < 3; ++i) {
          w[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          w[i].dstSet = set;
          w[i].dstBinding = i;
          w[i].descriptorCount = 1;
          w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
          w[i].pImageInfo = &imgs[i];
        }
        vkUpdateDescriptorSets(ctx.device->device(), 3, w, 0, nullptr);

        VkRenderingAttachmentInfo att{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        att.imageView = ctx.graph->image(composite).view;
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, {width, height}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &att;
        vkCmdBeginRendering(ctx.cmd, &rendering);
        VkViewport vp{0, 0, static_cast<f32>(width), static_cast<f32>(height), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {width, height}};
        vkCmdSetViewport(ctx.cmd, 0, 1, &vp);
        vkCmdSetScissor(ctx.cmd, 0, 1, &sc);
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, resolve_pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, resolve_layout_, 0, 1, &set,
                                0, nullptr);
        vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
        vkCmdEndRendering(ctx.cmd);
      });
  return composite;
}

void WboitPass::Destroy(Device& device) {
  if (geom_pipeline_) vkDestroyPipeline(device.device(), geom_pipeline_, nullptr);
  if (geom_layout_) vkDestroyPipelineLayout(device.device(), geom_layout_, nullptr);
  if (resolve_pipeline_) vkDestroyPipeline(device.device(), resolve_pipeline_, nullptr);
  if (resolve_layout_) vkDestroyPipelineLayout(device.device(), resolve_layout_, nullptr);
  if (resolve_set_layout_) vkDestroyDescriptorSetLayout(device.device(), resolve_set_layout_, nullptr);
  if (sampler_) vkDestroySampler(device.device(), sampler_, nullptr);
  device.DestroyBuffer(vertices_);
  device.DestroyBuffer(indices_);
}

}  // namespace rec::render
