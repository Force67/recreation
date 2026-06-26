#include "render/geometry/particles.h"

#include <algorithm>
#include <cstring>

#include "core/log.h"
#include "render/util/shader_util.h"
#include "shaders/particle_ps_hlsl.h"
#include "shaders/particle_sim_cs_hlsl.h"
#include "shaders/particle_vs_hlsl.h"

namespace rec::render {
namespace {

constexpr VkFormat kParticleMotionFormat = VK_FORMAT_R16G16_SFLOAT;  // == kMotionFormat

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
  Mat4 prev_view_proj;
};

struct ParticleSimPush {
  f32 emitter[3];
  f32 dt;
  f32 gravity;
  f32 spawn_speed;
  f32 life_min;
  f32 life_range;
  f32 size_min;
  f32 size_range;
  u32 count;
  u32 frame;
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

  // attachment 0 = lit colour, attachment 1 = motion. Both alpha-weighted so the
  // particle's velocity feeds the motion buffer where it is opaque.
  VkPipelineColorBlendAttachmentState blends[2]{};
  blends[0].blendEnable = VK_TRUE;
  blends[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  blends[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blends[0].colorBlendOp = VK_BLEND_OP_ADD;
  blends[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  blends[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blends[0].alphaBlendOp = VK_BLEND_OP_ADD;
  blends[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  blends[1] = blends[0];
  blends[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
  VkPipelineColorBlendStateCreateInfo blend_state{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend_state.attachmentCount = 2;
  blend_state.pAttachments = blends;

  VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamics;

  VkFormat color_formats[2] = {color_format, kParticleMotionFormat};
  VkPipelineRenderingCreateInfo rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 2;
  rendering.pColorAttachmentFormats = color_formats;

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

  // GPU simulation: a compute pipeline over the persistent state buffer.
  VkDescriptorSetLayoutBinding sim_bindings[2]{};
  for (u32 i = 0; i < 2; ++i) {
    sim_bindings[i].binding = i;
    sim_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sim_bindings[i].descriptorCount = 1;
    sim_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo sim_set_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  sim_set_info.bindingCount = 2;
  sim_set_info.pBindings = sim_bindings;
  if (vkCreateDescriptorSetLayout(device.device(), &sim_set_info, nullptr, &sim_set_layout_) !=
      VK_SUCCESS) {
    return false;
  }
  VkPushConstantRange sim_push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ParticleSimPush)};
  VkPipelineLayoutCreateInfo sim_layout_info{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  sim_layout_info.setLayoutCount = 1;
  sim_layout_info.pSetLayouts = &sim_set_layout_;
  sim_layout_info.pushConstantRangeCount = 1;
  sim_layout_info.pPushConstantRanges = &sim_push;
  if (vkCreatePipelineLayout(device.device(), &sim_layout_info, nullptr, &sim_layout_) !=
      VK_SUCCESS) {
    return false;
  }
  VkShaderModule sim_module =
      CreateShaderModule(device.device(), k_particle_sim_cs_hlsl, sizeof(k_particle_sim_cs_hlsl));
  if (sim_module == VK_NULL_HANDLE) return false;
  VkComputePipelineCreateInfo sim_info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  sim_info.stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  sim_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  sim_info.stage.module = sim_module;
  sim_info.stage.pName = "main";
  sim_info.layout = sim_layout_;
  VkResult sim_r = vkCreateComputePipelines(device.device(), VK_NULL_HANDLE, 1, &sim_info, nullptr,
                                            &sim_pipeline_);
  vkDestroyShaderModule(device.device(), sim_module, nullptr);
  if (sim_r != VK_SUCCESS) {
    REC_ERROR("particle sim pipeline creation failed");
    return false;
  }

  // 64 bytes per state entry; zero-init so every particle's seed is 0 and spawns
  // on first touch.
  sim_state_ = device.CreateBuffer(static_cast<u64>(kMaxParticles) * 64,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
  if (!sim_state_.mapped) return false;
  std::memset(sim_state_.mapped, 0, static_cast<size_t>(kMaxParticles) * 64);
  return true;
}

void ParticleSystem::AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                                ResourceHandle motion,
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
        builder.Write(motion, ResourceUsage::kColorAttachment);
        builder.Read(depth, ResourceUsage::kSampledFragment);
      },
      [this, color, depth, motion, buffer, count, frame](PassContext& ctx) {
        RecordDraw(ctx, color, depth, motion, buffer, count, frame);
      });
}

void ParticleSystem::RecordDraw(PassContext& ctx, ResourceHandle color, ResourceHandle depth,
                                ResourceHandle motion, VkBuffer instances, u32 count,
                                const Frame& frame) {
  VkDescriptorSet set = ctx.allocate_set(set_layout_);
  VkDescriptorBufferInfo buffer_info{instances, 0, count * sizeof(ParticleInstance)};
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
  VkRenderingAttachmentInfo attachments[2];
  attachments[0] = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  attachments[0].imageView = target.view;
  attachments[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;  // blend over the lit scene
  attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachments[1] = attachments[0];
  attachments[1].imageView = ctx.graph->image(motion).view;  // blend velocity over the mvecs
  VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea = {{0, 0}, target.extent};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 2;
  rendering.pColorAttachments = attachments;
  vkCmdBeginRendering(ctx.cmd, &rendering);

  VkViewport vp{0, 0, static_cast<f32>(target.extent.width),
                static_cast<f32>(target.extent.height), 0.0f, 1.0f};
  VkRect2D scissor{{0, 0}, target.extent};
  vkCmdSetViewport(ctx.cmd, 0, 1, &vp);
  vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);
  vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
  vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0, nullptr);

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
  push.prev_view_proj = frame.prev_view_proj;
  vkCmdPushConstants(ctx.cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                     sizeof(push), &push);
  vkCmdDraw(ctx.cmd, 4, count, 0, 0);
  vkCmdEndRendering(ctx.cmd);
}

void ParticleSystem::SimulateAndDraw(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                                     ResourceHandle motion, const Sim& sim, const Frame& frame,
                                     u32 frame_slot) {
  u32 count = std::min(sim.count, kMaxParticles);
  if (count == 0) return;
  VkBuffer instances = buffers_[frame_slot].buffer;
  VkBuffer state = sim_state_.buffer;

  graph.AddPass(
      "gpu_particles",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Write(color, ResourceUsage::kColorAttachment);
        builder.Write(motion, ResourceUsage::kColorAttachment);
        builder.Read(depth, ResourceUsage::kSampledFragment);
      },
      [this, color, depth, motion, instances, state, count, sim, frame](PassContext& ctx) {
        // Step the simulation, then draw the freshly written billboards.
        VkDescriptorSet sim_set = ctx.allocate_set(sim_set_layout_);
        VkDescriptorBufferInfo infos[2] = {{state, 0, VK_WHOLE_SIZE},
                                           {instances, 0, count * sizeof(ParticleInstance)}};
        VkWriteDescriptorSet sim_writes[2];
        for (u32 i = 0; i < 2; ++i) {
          sim_writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          sim_writes[i].dstSet = sim_set;
          sim_writes[i].dstBinding = i;
          sim_writes[i].descriptorCount = 1;
          sim_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
          sim_writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(ctx.device->device(), 2, sim_writes, 0, nullptr);

        ParticleSimPush sp{};
        sp.emitter[0] = sim.emitter[0];
        sp.emitter[1] = sim.emitter[1];
        sp.emitter[2] = sim.emitter[2];
        sp.dt = sim.dt < 0.05f ? sim.dt : 0.05f;  // clamp hitches
        sp.gravity = sim.gravity;
        sp.spawn_speed = sim.spawn_speed;
        sp.life_min = sim.life_min;
        sp.life_range = sim.life_range;
        sp.size_min = sim.size_min;
        sp.size_range = sim.size_range;
        sp.count = count;
        sp.frame = 0x9e3779b9u ^ count;  // nonzero seed salt; per-particle index varies it
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sim_pipeline_);
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sim_layout_, 0, 1, &sim_set,
                                0, nullptr);
        vkCmdPushConstants(ctx.cmd, sim_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(sp), &sp);
        vkCmdDispatch(ctx.cmd, (count + 63) / 64, 1, 1);

        // The instance writes must be visible to the vertex pull; the state
        // writes to the next frame's sim (same queue, ordered).
        VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.dstStageMask =
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(ctx.cmd, &dep);

        RecordDraw(ctx, color, depth, motion, instances, count, frame);
      });
}

void ParticleSystem::Destroy(Device& device) {
  if (pipeline_) vkDestroyPipeline(device.device(), pipeline_, nullptr);
  if (layout_) vkDestroyPipelineLayout(device.device(), layout_, nullptr);
  if (set_layout_) vkDestroyDescriptorSetLayout(device.device(), set_layout_, nullptr);
  if (sim_pipeline_) vkDestroyPipeline(device.device(), sim_pipeline_, nullptr);
  if (sim_layout_) vkDestroyPipelineLayout(device.device(), sim_layout_, nullptr);
  if (sim_set_layout_) vkDestroyDescriptorSetLayout(device.device(), sim_set_layout_, nullptr);
  device.DestroyBuffer(sim_state_);
  for (u32 i = 0; i < kFramesInFlight; ++i) device.DestroyBuffer(buffers_[i]);
}

}  // namespace rec::render
