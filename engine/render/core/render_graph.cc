#include "render/core/render_graph.h"

#include "core/log.h"
#include "render/rhi/device.h"

namespace rec::render {
namespace {

// Bytes per texel for the formats the graph allocates, for the memory readout.
u32 FormatBytes(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R8_UNORM: return 1;
    case VK_FORMAT_R16G16_SFLOAT: return 4;
    case VK_FORMAT_R32_SFLOAT: return 4;
    case VK_FORMAT_D32_SFLOAT: return 4;
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_UNORM: return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 4;
  }
}

bool IsReadUsage(ResourceUsage usage) {
  return usage == ResourceUsage::kSampledFragment || usage == ResourceUsage::kSampledCompute ||
         usage == ResourceUsage::kSampledTaskMesh;
}

struct UsageState {
  VkImageLayout layout;
  VkPipelineStageFlags2 stage;
  VkAccessFlags2 access;
  bool is_write;
};

UsageState StateFor(ResourceUsage usage) {
  switch (usage) {
    case ResourceUsage::kColorAttachment:
      // Read covers LOAD_OP_LOAD of previous content.
      return {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
              true};
    case ResourceUsage::kDepthAttachment:
      return {VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
              VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                  VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
              true};
    case ResourceUsage::kSampledFragment:
      return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false};
    case ResourceUsage::kSampledCompute:
      return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false};
    case ResourceUsage::kSampledTaskMesh:
      return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false};
    case ResourceUsage::kStorageWrite:
      return {VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT, true};
  }
  return {VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
          VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, true};
}

VkImageUsageFlags ImageUsageFor(ResourceUsage usage) {
  switch (usage) {
    case ResourceUsage::kColorAttachment:
      return VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    case ResourceUsage::kDepthAttachment:
      return VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    case ResourceUsage::kSampledFragment:
    case ResourceUsage::kSampledCompute:
    case ResourceUsage::kSampledTaskMesh:
      return VK_IMAGE_USAGE_SAMPLED_BIT;
    case ResourceUsage::kStorageWrite:
      return VK_IMAGE_USAGE_STORAGE_BIT;
  }
  return 0;
}

VkImageAspectFlags AspectFor(VkFormat format) {
  return format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}

}  // namespace

TransientPool::~TransientPool() { Clear(); }

void TransientPool::BeginFrame() {
  for (Entry& entry : entries_) entry.in_use = false;
}

const GpuImage* TransientPool::Acquire(VkFormat format, VkExtent2D extent,
                                       VkImageUsageFlags usage) {
  for (Entry& entry : entries_) {
    if (entry.in_use || entry.image.format != format ||
        entry.image.extent.width != extent.width || entry.image.extent.height != extent.height ||
        entry.usage != usage) {
      continue;
    }
    entry.in_use = true;
    return &entry.image;
  }

  Entry entry;
  entry.image = device_.CreateImage2D(format, extent, usage, AspectFor(format));
  if (entry.image.image == VK_NULL_HANDLE) return nullptr;
  entry.usage = usage;
  entry.in_use = true;
  entries_.push_back(entry);
  return &entries_.back().image;
}

void TransientPool::Clear() {
  for (Entry& entry : entries_) device_.DestroyImage(entry.image);
  entries_.clear();
}

ResourceHandle RenderGraph::CreateTexture(const TransientTextureDesc& desc) {
  resources_.push_back({.desc = desc});
  return static_cast<ResourceHandle>(resources_.size());
}

ResourceHandle RenderGraph::ImportImage(std::string name, const GpuImage& image,
                                        VkImageLayout* layout) {
  Resource resource;
  resource.desc.name = std::move(name);
  resource.desc.format = image.format;
  resource.image = image;
  resource.imported = true;
  resource.external_layout = layout;
  resource.layout = *layout;
  // The previous frame's accesses are unknown to the graph; a full barrier
  // on first touch is the safe default for persistent images.
  resource.last_stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  resource.last_access = VK_ACCESS_2_MEMORY_WRITE_BIT;
  resource.last_was_write = true;
  resources_.push_back(resource);
  return static_cast<ResourceHandle>(resources_.size());
}

ResourceHandle RenderGraph::ImportBackbuffer(const GpuImage& image) {
  Resource resource;
  resource.desc.name = "backbuffer";
  resource.desc.format = image.format;
  resource.image = image;
  resource.imported = true;
  resource.is_backbuffer = true;
  resource.layout = VK_IMAGE_LAYOUT_UNDEFINED;
  resources_.push_back(resource);
  return static_cast<ResourceHandle>(resources_.size());
}

void RenderGraph::AddPass(std::string name, SetupFn setup, ExecuteFn execute) {
  Pass pass{.name = std::move(name), .builder = {}, .execute = std::move(execute), .barriers = {}};
  setup(pass.builder);
  passes_.push_back(std::move(pass));
}

bool RenderGraph::Compile(Device& device, TransientPool& pool) {
  // Transients get the union of every declared usage so one physical image
  // serves all passes that touch it.
  base::Vector<VkImageUsageFlags> usages(resources_.size());
  for (const Pass& pass : passes_) {
    for (const auto& access : pass.builder.accesses) {
      usages[access.handle - 1] |= ImageUsageFor(access.usage);
    }
  }
  for (size_t i = 0; i < resources_.size(); ++i) {
    Resource& resource = resources_[i];
    if (resource.imported) continue;
    const GpuImage* image = pool.Acquire(
        resource.desc.format, {resource.desc.width, resource.desc.height}, usages[i]);
    if (!image) {
      REC_ERROR("transient allocation failed for {}", resource.desc.name);
      return false;
    }
    resource.image = *image;
  }

  for (Pass& pass : passes_) {
    for (const auto& access : pass.builder.accesses) {
      Resource& resource = resources_[access.handle - 1];
      UsageState next = StateFor(access.usage);
      bool layout_change = resource.layout != next.layout;
      bool hazard = resource.last_was_write || next.is_write;
      if (!layout_change && !hazard) continue;

      VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      barrier.srcStageMask = resource.last_stage;
      barrier.srcAccessMask = resource.last_access;
      barrier.dstStageMask = next.stage;
      barrier.dstAccessMask = next.access;
      barrier.oldLayout = resource.layout;
      barrier.newLayout = next.layout;
      barrier.image = resource.image.image;
      barrier.subresourceRange = {AspectFor(resource.image.format), 0, 1, 0, 1};
      if (barrier.srcStageMask == VK_PIPELINE_STAGE_2_NONE) {
        // First touch of a transient: the pool may be recycling an image
        // the previous frame still reads, so order behind everything.
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
      }
      pass.barriers.push_back(barrier);

      resource.layout = next.layout;
      resource.last_stage = next.stage;
      resource.last_access = next.access;
      resource.last_was_write = next.is_write;
    }
  }

  for (Resource& resource : resources_) {
    if (resource.is_backbuffer) {
      VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      barrier.srcStageMask = resource.last_stage;
      barrier.srcAccessMask = resource.last_access;
      barrier.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
      barrier.oldLayout = resource.layout;
      barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      barrier.image = resource.image.image;
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      final_barriers_.push_back(barrier);
    }
    if (resource.external_layout) *resource.external_layout = resource.layout;
  }

  // Snapshot the compiled graph for the debug inspector.
  stats_ = Stats{};
  for (const Pass& pass : passes_) {
    Stats::Pass entry;
    entry.name = pass.name;
    for (const auto& access : pass.builder.accesses) {
      if (IsReadUsage(access.usage)) ++entry.reads; else ++entry.writes;
    }
    entry.barriers = static_cast<u32>(pass.barriers.size());
    stats_.barrier_count += entry.barriers;
    stats_.passes.push_back(std::move(entry));
  }
  for (const Resource& resource : resources_) {
    Stats::Resource entry;
    entry.name = resource.desc.name;
    entry.width = resource.desc.width;
    entry.height = resource.desc.height;
    entry.imported = resource.imported;
    entry.bytes = static_cast<u64>(resource.desc.width) * resource.desc.height *
                  FormatBytes(resource.desc.format);
    if (!resource.imported) {
      stats_.transient_bytes += entry.bytes;
      ++stats_.transient_count;
    }
    stats_.resources.push_back(std::move(entry));
  }
  return true;
}

void RenderGraph::Execute(PassContext& ctx) {
  ctx.graph = this;
  for (Pass& pass : passes_) {
    if (pass_begin_) pass_begin_(ctx.cmd, pass.name.c_str());
    if (!pass.barriers.empty()) {
      VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.imageMemoryBarrierCount = static_cast<u32>(pass.barriers.size());
      dep.pImageMemoryBarriers = pass.barriers.data();
      vkCmdPipelineBarrier2(ctx.cmd, &dep);
    }
    if (pass.execute) pass.execute(ctx);
    if (pass_end_) pass_end_(ctx.cmd);
  }
  if (!final_barriers_.empty()) {
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = static_cast<u32>(final_barriers_.size());
    dep.pImageMemoryBarriers = final_barriers_.data();
    vkCmdPipelineBarrier2(ctx.cmd, &dep);
  }
}

void RenderGraph::Reset() {
  resources_.clear();
  passes_.clear();
  final_barriers_.clear();
}

}  // namespace rec::render
