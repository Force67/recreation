#ifndef RECREATION_RENDER_RENDER_GRAPH_H_
#define RECREATION_RENDER_RENDER_GRAPH_H_

#include <functional>
#include <string>
#include <vector>

#include "core/types.h"
#include "render/rhi/resources.h"

namespace rec::render {

class Device;
class RenderGraph;

using ResourceHandle = u32;
constexpr ResourceHandle kInvalidResource = 0;

// How a pass touches a resource. Each usage maps to exactly one image
// layout, pipeline stage and access mask, which is all the graph needs to
// derive barriers.
enum class ResourceUsage : u8 {
  kColorAttachment,
  kDepthAttachment,
  kSampledFragment,
  kSampledCompute,
  kStorageWrite,
};

struct TransientTextureDesc {
  std::string name;
  VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
  u32 width = 0;
  u32 height = 0;
};

// Caches images keyed by their description so a steady frame allocates
// nothing. Contents are not preserved across frames: every acquisition
// starts in UNDEFINED and the first barrier discards.
class TransientPool {
 public:
  explicit TransientPool(Device& device) : device_(device) {}
  ~TransientPool();

  TransientPool(const TransientPool&) = delete;
  TransientPool& operator=(const TransientPool&) = delete;

  void BeginFrame();
  const GpuImage* Acquire(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage);

  // Frees every cached image. Call after WaitIdle, e.g. on resize.
  void Clear();

 private:
  struct Entry {
    GpuImage image;
    VkImageUsageFlags usage = 0;
    bool in_use = false;
  };

  Device& device_;
  std::vector<Entry> entries_;
};

// Handed to pass execute callbacks. Descriptor sets come from a per frame
// pool owned by the renderer, valid until that frame slot is reused.
struct PassContext {
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  Device* device = nullptr;
  RenderGraph* graph = nullptr;
  std::function<VkDescriptorSet(VkDescriptorSetLayout)> allocate_set;
};

// Declared per frame, compiled into barriers and executed into the frame
// command buffer. Passes declare reads and writes with a usage; Compile
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
    std::vector<Access> accesses;
  };

  using SetupFn = std::function<void(PassBuilder&)>;
  using ExecuteFn = std::function<void(PassContext&)>;

  ResourceHandle CreateTexture(const TransientTextureDesc& desc);

  // Persistent images (TAA history, swapchain) enter the graph here. The
  // graph reads the starting layout from *layout and writes the layout the
  // image is left in back to it, so the owner can re-import next frame.
  ResourceHandle ImportImage(std::string name, const GpuImage& image, VkImageLayout* layout);

  // Imported swapchain image. Contents are discarded on first use and the
  // graph appends a transition to PRESENT_SRC after the last pass.
  ResourceHandle ImportBackbuffer(const GpuImage& image);

  void AddPass(std::string name, SetupFn setup, ExecuteFn execute);

  bool Compile(Device& device, TransientPool& pool);
  void Execute(PassContext& ctx);
  void Reset();

  const GpuImage& image(ResourceHandle handle) const { return resources_[handle - 1].image; }

 private:
  struct Resource {
    TransientTextureDesc desc;
    GpuImage image;
    bool imported = false;
    bool is_backbuffer = false;
    VkImageLayout* external_layout = nullptr;
    // Walked during Compile to derive barriers.
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 last_stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 last_access = VK_ACCESS_2_NONE;
    bool last_was_write = false;
  };

  struct Pass {
    std::string name;
    PassBuilder builder;
    ExecuteFn execute;
    std::vector<VkImageMemoryBarrier2> barriers;
  };

  std::vector<Resource> resources_;
  std::vector<Pass> passes_;
  std::vector<VkImageMemoryBarrier2> final_barriers_;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_RENDER_GRAPH_H_
