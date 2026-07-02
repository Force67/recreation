#include "render/core/render_graph.h"

#include "core/log.h"
#include "render/rhi/device.h"

namespace rec::render {
namespace {

bool IsReadUsage(ResourceUsage usage) {
  return usage == ResourceUsage::kSampledFragment || usage == ResourceUsage::kSampledCompute ||
         usage == ResourceUsage::kSampledTaskMesh;
}

struct UsageState {
  ResourceState state;
  bool is_write;
};

UsageState StateFor(ResourceUsage usage) {
  switch (usage) {
    case ResourceUsage::kColorAttachment:
      // Write covers LOAD_OP_LOAD of previous content too.
      return {ResourceState::kColorTarget, true};
    case ResourceUsage::kDepthAttachment:
      return {ResourceState::kDepthTarget, true};
    case ResourceUsage::kSampledFragment:
      return {ResourceState::kShaderReadFragment, false};
    case ResourceUsage::kSampledCompute:
      return {ResourceState::kShaderReadCompute, false};
    case ResourceUsage::kSampledTaskMesh:
      return {ResourceState::kShaderReadTaskMesh, false};
    case ResourceUsage::kStorageWrite:
      return {ResourceState::kGeneral, true};
  }
  return {ResourceState::kGeneral, true};
}

TextureUsageFlags ImageUsageFor(ResourceUsage usage) {
  switch (usage) {
    case ResourceUsage::kColorAttachment:
      return kTextureUsageColorTarget;
    case ResourceUsage::kDepthAttachment:
      return kTextureUsageDepthTarget;
    case ResourceUsage::kSampledFragment:
    case ResourceUsage::kSampledCompute:
    case ResourceUsage::kSampledTaskMesh:
      return kTextureUsageSampled;
    case ResourceUsage::kStorageWrite:
      return kTextureUsageStorage;
  }
  return 0;
}

}  // namespace

TransientPool::~TransientPool() { Clear(); }

void TransientPool::BeginFrame() {
  for (Entry& entry : entries_) entry.in_use = false;
}

const GpuImage* TransientPool::Acquire(Format format, Extent2D extent, TextureUsageFlags usage) {
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
  entry.image = device_.CreateImage2D(format, extent, usage);
  if (!entry.image) return nullptr;
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
                                        ResourceState* state) {
  Resource resource;
  resource.desc.name = std::move(name);
  resource.desc.format = image.format;
  resource.image = image;
  resource.imported = true;
  resource.external_state = state;
  resource.state = *state;
  // The previous frame's accesses are unknown to the graph; treat the first
  // touch as following a write so a hazard barrier is always emitted.
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
  resource.state = ResourceState::kUndefined;
  resource.last_was_write = true;
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
  base::Vector<TextureUsageFlags> usages(resources_.size());
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
      bool state_change = resource.state != next.state;
      bool hazard = resource.last_was_write || next.is_write;
      if (!state_change && !hazard) continue;

      pass.barriers.push_back({.texture = resource.image.handle,
                               .before = resource.state,
                               .after = next.state});
      resource.state = next.state;
      resource.last_was_write = next.is_write;
    }
  }

  for (Resource& resource : resources_) {
    if (resource.is_backbuffer) {
      final_barriers_.push_back({.texture = resource.image.handle,
                                 .before = resource.state,
                                 .after = ResourceState::kPresent});
    }
    if (resource.external_state) *resource.external_state = resource.state;
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
                  FormatTexelBytes(resource.desc.format);
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
    if (pass_begin_) pass_begin_(*ctx.cmd, pass.name.c_str());
    if (!pass.barriers.empty()) {
      ctx.cmd->TextureBarriers({pass.barriers.data(), pass.barriers.size()});
    }
    if (pass.execute) pass.execute(ctx);
    if (pass_end_) pass_end_(*ctx.cmd);
  }
  if (!final_barriers_.empty()) {
    ctx.cmd->TextureBarriers({final_barriers_.data(), final_barriers_.size()});
  }
}

void RenderGraph::Reset() {
  resources_.clear();
  passes_.clear();
  final_barriers_.clear();
}

}  // namespace rec::render
