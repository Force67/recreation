#ifndef RECREATION_RENDER_RENDER_GRAPH_H_
#define RECREATION_RENDER_RENDER_GRAPH_H_

#include <functional>
#include <string>

#include <base/containers/vector.h>

#include "core/types.h"
#include "render/rhi/command_list.h"
#include "render/rhi/resources.h"

namespace rec::render {

class Device;
class RenderGraph;

using ResourceHandle = u32;
constexpr ResourceHandle kInvalidResource = 0;

// How a pass touches a resource. Each usage maps to exactly one ResourceState
// (and image usage flag), which is all the graph needs to derive barriers.
enum class ResourceUsage : u8 {
  kColorAttachment,
  kDepthAttachment,
  kSampledFragment,
  kSampledCompute,
  kSampledTaskMesh,  // sampled in the task/mesh stages (mesh-shader cull)
  kStorageWrite,
};

struct TransientTextureDesc {
  std::string name;
  Format format = Format::kRGBA16Float;
  u32 width = 0;
  u32 height = 0;
};

// Caches images keyed by their description so a steady frame allocates
// nothing. Contents are not preserved across frames: every acquisition
// starts in kUndefined and the first barrier discards.
class TransientPool {
 public:
  explicit TransientPool(Device& device) : device_(device) {}
  ~TransientPool();

  TransientPool(const TransientPool&) = delete;
  TransientPool& operator=(const TransientPool&) = delete;

  void BeginFrame();
  const GpuImage* Acquire(Format format, Extent2D extent, TextureUsageFlags usage);

  // Frees every cached image. Call after WaitIdle, e.g. on resize.
  void Clear();

 private:
  struct Entry {
    GpuImage image;
    TextureUsageFlags usage = 0;
    bool in_use = false;
  };

  Device& device_;
  base::Vector<Entry> entries_;
};

// Handed to pass execute callbacks.
struct PassContext {
  CommandList* cmd = nullptr;
  Device* device = nullptr;
  RenderGraph* graph = nullptr;
};

// Declared per frame, compiled into barriers and executed into the frame
// command list. Passes declare reads and writes with a usage; Compile
// assigns physical images from the transient pool and derives the image
// barriers between passes from the declared accesses.
class RenderGraph {
 public:
  struct PassBuilder {
    void Read(ResourceHandle handle, ResourceUsage usage) { accesses.push_back({handle, usage}); }
    void Write(ResourceHandle handle, ResourceUsage usage) { accesses.push_back({handle, usage}); }
    struct Access {
      ResourceHandle handle;
      ResourceUsage usage;
    };
    base::Vector<Access> accesses;
  };

  using SetupFn = std::function<void(PassBuilder&)>;
  using ExecuteFn = std::function<void(PassContext&)>;

  ResourceHandle CreateTexture(const TransientTextureDesc& desc);

  // Persistent images (TAA history, path-trace ping-pongs) enter the graph
  // here. The graph reads the starting state from *state and writes the state
  // the image is left in back to it, so the owner can re-import next frame.
  ResourceHandle ImportImage(std::string name, const GpuImage& image, ResourceState* state);

  // Imported swapchain image. Contents are discarded on first use and the
  // graph appends a transition to kPresent after the last pass.
  ResourceHandle ImportBackbuffer(const GpuImage& image);

  void AddPass(std::string name, SetupFn setup, ExecuteFn execute);

  // Optional per-pass brackets (gpu profiler timestamps + debug labels). Begin
  // runs before the pass barriers, end after the pass executes.
  using PassBegin = std::function<void(CommandList&, const char*)>;
  using PassEnd = std::function<void(CommandList&)>;
  void SetPassHooks(PassBegin begin, PassEnd end) {
    pass_begin_ = std::move(begin);
    pass_end_ = std::move(end);
  }

  bool Compile(Device& device, TransientPool& pool);
  void Execute(PassContext& ctx);
  void Reset();

  const GpuImage& image(ResourceHandle handle) const { return resources_[handle - 1].image; }

  // A snapshot of the compiled frame graph for the debug inspector: passes with
  // their read/write/barrier counts, transient resources with their footprint,
  // and totals. Captured at the end of Compile.
  struct Stats {
    struct Pass {
      std::string name;
      u32 reads = 0;
      u32 writes = 0;
      u32 barriers = 0;
    };
    struct Resource {
      std::string name;
      u32 width = 0;
      u32 height = 0;
      u64 bytes = 0;
      bool imported = false;
    };
    base::Vector<Pass> passes;
    base::Vector<Resource> resources;
    u64 transient_bytes = 0;
    u32 transient_count = 0;
    u32 barrier_count = 0;
  };
  const Stats& stats() const { return stats_; }

 private:
  struct Resource {
    TransientTextureDesc desc;
    GpuImage image;
    bool imported = false;
    bool is_backbuffer = false;
    ResourceState* external_state = nullptr;
    // Walked during Compile to derive barriers.
    ResourceState state = ResourceState::kUndefined;
    bool last_was_write = false;
  };

  struct Pass {
    std::string name;
    PassBuilder builder;
    ExecuteFn execute;
    base::Vector<TextureBarrier> barriers;
  };

  base::Vector<Resource> resources_;
  base::Vector<Pass> passes_;
  base::Vector<TextureBarrier> final_barriers_;
  Stats stats_;
  PassBegin pass_begin_;
  PassEnd pass_end_;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_RENDER_GRAPH_H_
