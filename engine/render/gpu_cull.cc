#include "render/gpu_cull.h"

#include <cmath>
#include <cstring>

#include "core/log.h"
#include "render/shader_util.h"
#include "shaders/cull_cs_hlsl.h"

namespace rec::render {
namespace {

struct CullPush {
  f32 planes[5][4];
  u32 instance_count;
  u32 cull_enabled;
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

bool GpuCull::Initialize(Device& device) {
  VkDescriptorSetLayoutBinding bindings[3]{};
  for (u32 i = 0; i < 3; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo set_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 3;
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
  return true;
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

void GpuCull::AddToGraph(RenderGraph& graph, const Mat4& view_proj, u32 instance_count, bool enabled,
                         u32 slot) {
  if (instance_count == 0) return;
  *static_cast<u32*>(counts_[slot].mapped) = 0;  // reset the visible counter

  CullPush push{};
  ExtractPlanes(view_proj, push.planes);
  push.instance_count = instance_count;
  push.cull_enabled = enabled ? 1u : 0u;
  VkBuffer instances = instances_[slot].buffer;
  VkBuffer commands = commands_[slot].buffer;
  VkBuffer counts = counts_[slot].buffer;

  graph.AddPass(
      "cull", [](RenderGraph::PassBuilder&) {},
      [this, push, instances, commands, counts, instance_count](PassContext& ctx) {
        VkDescriptorSet set = ctx.allocate_set(set_layout_);
        VkDescriptorBufferInfo infos[3] = {{instances, 0, VK_WHOLE_SIZE},
                                           {commands, 0, VK_WHOLE_SIZE},
                                           {counts, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[3];
        for (u32 i = 0; i < 3; ++i) {
          writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          writes[i].dstSet = set;
          writes[i].dstBinding = i;
          writes[i].descriptorCount = 1;
          writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
          writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(ctx.device->device(), 3, writes, 0, nullptr);

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
  for (u32 i = 0; i < kFramesInFlight; ++i) {
    device.DestroyBuffer(instances_[i]);
    device.DestroyBuffer(commands_[i]);
    device.DestroyBuffer(counts_[i]);
  }
}

}  // namespace rec::render
