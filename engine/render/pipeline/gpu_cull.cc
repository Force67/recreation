#include "render/pipeline/gpu_cull.h"

#include <cmath>
#include <cstring>

#include "core/log.h"
#include "render/rhi/device.h"
#include "render/util/shader_util.h"
#include "shaders/bounds_ps_hlsl.h"
#include "shaders/bounds_vs_hlsl.h"
#include "shaders/cull_cs_hlsl.h"
#include "shaders/hiz_reduce_cs_hlsl.h"

namespace rec::render {
namespace {

struct CullPush {
  f32 planes[5][4];   // 80
  Mat4 prev_view_proj;
  f32 eye_pad[4];     // xyz eye
  f32 proj_hiz[4];    // proj.m00, proj.m11, hiz w, hiz h
  u32 misc[4];        // instance_count, frustum_enabled, occlusion_enabled, pad
};

struct HizPush {
  u32 dst_size[2];
  u32 block;
};

// Gribb-Hartmann frustum planes from a column-major view_proj (clip = vp*world).
// Normalized, oriented so a point is inside when dot(n, p) + d >= 0. Returns the
// four side planes plus near; the reversed-z far plane is skipped (conservative).
void ExtractPlanes(const Mat4& vp, f32 out[5][4]) {
  const f32* m = vp.m;
  auto row = [&](int r, int c) { return m[c * 4 + r]; };
  // left, right, bottom, top, near
  f32 p[5][4] = {
      {row(3, 0) + row(0, 0), row(3, 1) + row(0, 1), row(3, 2) + row(0, 2), row(3, 3) + row(0, 3)},
      {row(3, 0) - row(0, 0), row(3, 1) - row(0, 1), row(3, 2) - row(0, 2), row(3, 3) - row(0, 3)},
      {row(3, 0) + row(1, 0), row(3, 1) + row(1, 1), row(3, 2) + row(1, 2), row(3, 3) + row(1, 3)},
      {row(3, 0) - row(1, 0), row(3, 1) - row(1, 1), row(3, 2) - row(1, 2), row(3, 3) - row(1, 3)},
      {row(2, 0), row(2, 1), row(2, 2), row(2, 3)},  // near: clip.z >= 0
  };
  for (int i = 0; i < 5; ++i) {
    f32 len = std::sqrt(p[i][0] * p[i][0] + p[i][1] * p[i][1] + p[i][2] * p[i][2]);
    if (len < 1e-8f) len = 1.0f;
    for (int c = 0; c < 4; ++c) out[i][c] = p[i][c] / len;
  }
}

}  // namespace

bool GpuCull::Initialize(Device& device, VkFormat color_format) {
  VkDescriptorSetLayoutBinding bindings[4]{};
  for (u32 i = 0; i < 3; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  bindings[3].binding = 3;  // hi-z (occlusion)
  bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  bindings[3].descriptorCount = 1;
  bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo set_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 4;
  set_info.pBindings = bindings;
  if (vkCreateDescriptorSetLayout(device.device(), &set_info, nullptr, &set_layout_) != VK_SUCCESS) {
    return false;
  }

  VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CullPush)};
  VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push;
  if (vkCreatePipelineLayout(device.device(), &layout_info, nullptr, &layout_) != VK_SUCCESS) {
    return false;
  }

  VkShaderModule module = CreateShaderModule(device.device(), k_cull_cs_hlsl, sizeof(k_cull_cs_hlsl));
  if (module == VK_NULL_HANDLE) return false;
  VkComputePipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  info.stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  info.stage.module = module;
  info.stage.pName = "main";
  info.layout = layout_;
  VkResult r =
      vkCreateComputePipelines(device.device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_);
  vkDestroyShaderModule(device.device(), module, nullptr);
  if (r != VK_SUCCESS) {
    REC_ERROR("cull pipeline creation failed");
    return false;
  }

  for (u32 i = 0; i < kFramesInFlight; ++i) {
    instances_[i] = device.CreateBuffer(static_cast<u64>(kMaxInstances) * sizeof(Instance),
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    commands_[i] = device.CreateBuffer(
        static_cast<u64>(kMaxCommands) * sizeof(Command),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, true);
    counts_[i] = device.CreateBuffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    if (!instances_[i].mapped || !commands_[i].mapped || !counts_[i].mapped) return false;
  }

  // Hi-z reduce pipeline: storage dst + sampled src.
  VkDescriptorSetLayoutBinding hb[2]{};
  hb[0] = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
           .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
  hb[1] = {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = 1,
           .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
  VkDescriptorSetLayoutCreateInfo hsi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  hsi.bindingCount = 2;
  hsi.pBindings = hb;
  if (vkCreateDescriptorSetLayout(device.device(), &hsi, nullptr, &hiz_set_layout_) != VK_SUCCESS)
    return false;
  VkPushConstantRange hpr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(HizPush)};
  VkPipelineLayoutCreateInfo hli{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  hli.setLayoutCount = 1;
  hli.pSetLayouts = &hiz_set_layout_;
  hli.pushConstantRangeCount = 1;
  hli.pPushConstantRanges = &hpr;
  if (vkCreatePipelineLayout(device.device(), &hli, nullptr, &hiz_layout_) != VK_SUCCESS)
    return false;
  VkShaderModule hm =
      CreateShaderModule(device.device(), k_hiz_reduce_cs_hlsl, sizeof(k_hiz_reduce_cs_hlsl));
  if (hm == VK_NULL_HANDLE) return false;
  VkComputePipelineCreateInfo hci{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  hci.stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  hci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  hci.stage.module = hm;
  hci.stage.pName = "main";
  hci.layout = hiz_layout_;
  VkResult hr =
      vkCreateComputePipelines(device.device(), VK_NULL_HANDLE, 1, &hci, nullptr, &hiz_pipeline_);
  vkDestroyShaderModule(device.device(), hm, nullptr);
  if (hr != VK_SUCCESS) return false;

  return CreateBoundsPipeline(device, color_format);
}

void GpuCull::ResizeDepth(Device& device, u32 width, u32 height) {
  if (width == depth_w_ && height == depth_h_) return;
  for (u32 i = 0; i < kFramesInFlight; ++i) {
    device.DestroyImage(prev_depth_[i]);
    prev_depth_[i] = device.CreateImage2D(
        VK_FORMAT_R32_SFLOAT, {width, height},
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    prev_depth_layout_[i] = VK_IMAGE_LAYOUT_UNDEFINED;
  }
  depth_w_ = width;
  depth_h_ = height;
  hiz_w_ = (width + kHizDownsample - 1) / kHizDownsample;
  hiz_h_ = (height + kHizDownsample - 1) / kHizDownsample;
}

ResourceHandle GpuCull::BuildHiZ(RenderGraph& graph, u32 slot) {
  if (depth_w_ == 0) return kInvalidResource;
  u32 read = (slot + 1) % kFramesInFlight;  // last frame's snapshot
  ResourceHandle prev = graph.ImportImage("cull_prev_depth", prev_depth_[read],
                                          &prev_depth_layout_[read]);
  ResourceHandle hiz =
      graph.CreateTexture({.name = "cull_hiz", .format = VK_FORMAT_R32_SFLOAT,
                           .width = hiz_w_, .height = hiz_h_});
  graph.AddPass(
      "cull_hiz",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(prev, ResourceUsage::kSampledCompute);
        builder.Write(hiz, ResourceUsage::kStorageWrite);
      },
      [this, prev, hiz](PassContext& ctx) {
        VkDescriptorSet set = ctx.allocate_set(hiz_set_layout_);
        VkDescriptorImageInfo imgs[2] = {
            {VK_NULL_HANDLE, ctx.graph->image(hiz).view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, ctx.graph->image(prev).view,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet w[2];
        w[0] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[0].dstSet = set;
        w[0].dstBinding = 0;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[0].pImageInfo = &imgs[0];
        w[1] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[1].dstSet = set;
        w[1].dstBinding = 1;
        w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w[1].pImageInfo = &imgs[1];
        vkUpdateDescriptorSets(ctx.device->device(), 2, w, 0, nullptr);
        HizPush push{{hiz_w_, hiz_h_}, kHizDownsample};
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hiz_pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hiz_layout_, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(ctx.cmd, hiz_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                           &push);
        vkCmdDispatch(ctx.cmd, (hiz_w_ + 7) / 8, (hiz_h_ + 7) / 8, 1);
      });
  return hiz;
}

void GpuCull::CopyDepth(RenderGraph& graph, ResourceHandle depth_export, u32 slot) {
  if (depth_w_ == 0) return;
  ResourceHandle dst =
      graph.ImportImage("cull_depth_snapshot", prev_depth_[slot], &prev_depth_layout_[slot]);
  graph.AddPass(
      "cull_depth_copy",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(depth_export, ResourceUsage::kSampledCompute);
        builder.Write(dst, ResourceUsage::kStorageWrite);
      },
      [this, depth_export, dst](PassContext& ctx) {
        VkDescriptorSet set = ctx.allocate_set(hiz_set_layout_);
        VkDescriptorImageInfo imgs[2] = {
            {VK_NULL_HANDLE, ctx.graph->image(dst).view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, ctx.graph->image(depth_export).view,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet w[2];
        w[0] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[0].dstSet = set;
        w[0].dstBinding = 0;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[0].pImageInfo = &imgs[0];
        w[1] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[1].dstSet = set;
        w[1].dstBinding = 1;
        w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w[1].pImageInfo = &imgs[1];
        vkUpdateDescriptorSets(ctx.device->device(), 2, w, 0, nullptr);
        HizPush push{{depth_w_, depth_h_}, 1};  // 1:1 snapshot
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hiz_pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hiz_layout_, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(ctx.cmd, hiz_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                           &push);
        vkCmdDispatch(ctx.cmd, (depth_w_ + 7) / 8, (depth_h_ + 7) / 8, 1);
      });
}

bool GpuCull::CreateBoundsPipeline(Device& device, VkFormat color_format) {
  VkDescriptorSetLayoutBinding binding{};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  VkDescriptorSetLayoutCreateInfo set_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 1;
  set_info.pBindings = &binding;
  if (vkCreateDescriptorSetLayout(device.device(), &set_info, nullptr, &bounds_set_layout_) !=
      VK_SUCCESS) {
    return false;
  }

  VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4)};
  VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &bounds_set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push;
  if (vkCreatePipelineLayout(device.device(), &layout_info, nullptr, &bounds_layout_) !=
      VK_SUCCESS) {
    return false;
  }

  VkShaderModule vs =
      CreateShaderModule(device.device(), k_bounds_vs_hlsl, sizeof(k_bounds_vs_hlsl));
  VkShaderModule ps =
      CreateShaderModule(device.device(), k_bounds_ps_hlsl, sizeof(k_bounds_ps_hlsl));
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
  ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
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
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};  // overlay, no depth
  VkPipelineColorBlendAttachmentState blend{};
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
  info.layout = bounds_layout_;
  VkResult r = vkCreateGraphicsPipelines(device.device(), VK_NULL_HANDLE, 1, &info, nullptr,
                                         &bounds_pipeline_);
  vkDestroyShaderModule(device.device(), vs, nullptr);
  vkDestroyShaderModule(device.device(), ps, nullptr);
  if (r != VK_SUCCESS) {
    REC_ERROR("bounds pipeline creation failed");
    return false;
  }
  return true;
}

void GpuCull::AddBoundsPass(RenderGraph& graph, ResourceHandle color, const Mat4& view_proj,
                            u32 instance_count, u32 slot) {
  if (instance_count == 0) return;
  VkBuffer instances = instances_[slot].buffer;
  graph.AddPass(
      "bounds_debug",
      [&](RenderGraph::PassBuilder& builder) { builder.Write(color, ResourceUsage::kColorAttachment); },
      [this, color, instances, view_proj, instance_count](PassContext& ctx) {
        VkDescriptorSet set = ctx.allocate_set(bounds_set_layout_);
        VkDescriptorBufferInfo info{instances, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &info;
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
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bounds_pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bounds_layout_, 0, 1, &set,
                                0, nullptr);
        vkCmdPushConstants(ctx.cmd, bounds_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4),
                           &view_proj);
        vkCmdDraw(ctx.cmd, 24, instance_count, 0, 0);
        vkCmdEndRendering(ctx.cmd);
      });
}

GpuCull::Instance* GpuCull::instances(u32 slot) {
  return static_cast<Instance*>(instances_[slot].mapped);
}

GpuCull::Command* GpuCull::commands(u32 slot) {
  return static_cast<Command*>(commands_[slot].mapped);
}

u32 GpuCull::last_visible(u32 slot) const {
  return *static_cast<const u32*>(counts_[slot].mapped);
}

void GpuCull::AddToGraph(RenderGraph& graph, const Mat4& view_proj, const Mat4& prev_view_proj,
                         const f32 proj_scale[2], const Vec3& eye, u32 instance_count, bool frustum,
                         bool occlusion, ResourceHandle hiz, u32 slot) {
  if (instance_count == 0) return;
  *static_cast<u32*>(counts_[slot].mapped) = 0;  // reset the visible counter

  bool occ = occlusion && hiz != kInvalidResource;
  CullPush push{};
  ExtractPlanes(view_proj, push.planes);
  push.prev_view_proj = prev_view_proj;
  push.eye_pad[0] = eye.x;
  push.eye_pad[1] = eye.y;
  push.eye_pad[2] = eye.z;
  push.proj_hiz[0] = proj_scale[0];
  push.proj_hiz[1] = proj_scale[1];
  push.proj_hiz[2] = static_cast<f32>(hiz_w_);
  push.proj_hiz[3] = static_cast<f32>(hiz_h_);
  push.misc[0] = instance_count;
  push.misc[1] = frustum ? 1u : 0u;
  push.misc[2] = occ ? 1u : 0u;
  VkBuffer instances = instances_[slot].buffer;
  VkBuffer commands = commands_[slot].buffer;
  VkBuffer counts = counts_[slot].buffer;

  bool has_hiz = hiz != kInvalidResource;
  graph.AddPass(
      "cull",
      [&](RenderGraph::PassBuilder& builder) {
        if (has_hiz) builder.Read(hiz, ResourceUsage::kSampledCompute);
      },
      [this, push, instances, commands, counts, instance_count, has_hiz, hiz](PassContext& ctx) {
        VkDescriptorSet set = ctx.allocate_set(set_layout_);
        VkDescriptorBufferInfo infos[3] = {{instances, 0, VK_WHOLE_SIZE},
                                           {commands, 0, VK_WHOLE_SIZE},
                                           {counts, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[4];
        for (u32 i = 0; i < 3; ++i) {
          writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          writes[i].dstSet = set;
          writes[i].dstBinding = i;
          writes[i].descriptorCount = 1;
          writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
          writes[i].pBufferInfo = &infos[i];
        }
        VkDescriptorImageInfo hiz_info{VK_NULL_HANDLE, VK_NULL_HANDLE,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        u32 write_count = 3;
        if (has_hiz) {
          hiz_info.imageView = ctx.graph->image(hiz).view;
          writes[3] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          writes[3].dstSet = set;
          writes[3].dstBinding = 3;
          writes[3].descriptorCount = 1;
          writes[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
          writes[3].pImageInfo = &hiz_info;
          write_count = 4;
        }
        vkUpdateDescriptorSets(ctx.device->device(), write_count, writes, 0, nullptr);

        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(ctx.cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(ctx.cmd, (instance_count + 63) / 64, 1, 1);

        // Make the written instanceCounts visible to the indirect draws.
        VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(ctx.cmd, &dep);
      });
}

void GpuCull::Destroy(Device& device) {
  if (pipeline_) vkDestroyPipeline(device.device(), pipeline_, nullptr);
  if (layout_) vkDestroyPipelineLayout(device.device(), layout_, nullptr);
  if (set_layout_) vkDestroyDescriptorSetLayout(device.device(), set_layout_, nullptr);
  if (bounds_pipeline_) vkDestroyPipeline(device.device(), bounds_pipeline_, nullptr);
  if (bounds_layout_) vkDestroyPipelineLayout(device.device(), bounds_layout_, nullptr);
  if (bounds_set_layout_) vkDestroyDescriptorSetLayout(device.device(), bounds_set_layout_, nullptr);
  if (hiz_pipeline_) vkDestroyPipeline(device.device(), hiz_pipeline_, nullptr);
  if (hiz_layout_) vkDestroyPipelineLayout(device.device(), hiz_layout_, nullptr);
  if (hiz_set_layout_) vkDestroyDescriptorSetLayout(device.device(), hiz_set_layout_, nullptr);
  for (u32 i = 0; i < kFramesInFlight; ++i) {
    device.DestroyBuffer(instances_[i]);
    device.DestroyBuffer(commands_[i]);
    device.DestroyBuffer(counts_[i]);
    device.DestroyImage(prev_depth_[i]);
  }
}

}  // namespace rec::render
