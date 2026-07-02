#ifndef RECREATION_RENDER_VULKAN_VK_BACKEND_H_
#define RECREATION_RENDER_VULKAN_VK_BACKEND_H_

// Internal Vulkan backend types. Only vulkan/*.cc and rhi/vulkan_interop.h may
// include this; pass code sees rhi/ headers exclusively.

#include <volk.h>

#include <vk_mem_alloc.h>

#include <memory>

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "render/rhi/device.h"
#include "render/rhi/swapchain.h"

namespace rec::render::vk {

// --- backend records behind the opaque handles ---

struct BufferRecord {
  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;
};

struct TextureRecord {
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;  // null for swapchain images
  VkImageView view = VK_NULL_HANDLE;   // whole-image default view
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  u32 mip_levels = 1;
  u32 array_layers = 1;
};

struct BindingLayoutRecord {
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  BindingLayoutDesc desc;  // kept for pool sizing of persistent sets
};

struct BindingSetRecord {
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;  // dedicated pool, owned
  BindingLayoutRecord* layout = nullptr;
};

struct PipelineRecord {
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;  // cached, not owned
  VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_COMPUTE;
  VkShaderStageFlags push_stages = 0;
  u32 push_size = 0;
  // Cached set layouts for BindTransient, indexed by set. Not owned.
  base::Vector<VkDescriptorSetLayout> set_layouts;
};

struct AccelStructRecord {
  VkAccelerationStructureKHR accel = VK_NULL_HANDLE;
  BufferRecord backing;
  u64 address = 0;
};

struct TimestampPoolRecord {
  VkQueryPool pool = VK_NULL_HANDLE;
  u32 count = 0;
};

// Handle <-> record casts. Handles are record pointers; TextureView and
// SamplerHandle carry the Vulkan handle bits directly (they are immutable and
// need no metadata).
inline BufferRecord* Rec(BufferHandle h) { return reinterpret_cast<BufferRecord*>(h.value); }
inline TextureRecord* Rec(TextureHandle h) { return reinterpret_cast<TextureRecord*>(h.value); }
inline BindingLayoutRecord* Rec(BindingLayoutHandle h) {
  return reinterpret_cast<BindingLayoutRecord*>(h.value);
}
inline BindingSetRecord* Rec(BindingSetHandle h) {
  return reinterpret_cast<BindingSetRecord*>(h.value);
}
inline PipelineRecord* Rec(PipelineHandle h) { return reinterpret_cast<PipelineRecord*>(h.value); }
inline AccelStructRecord* Rec(AccelStructHandle h) {
  return reinterpret_cast<AccelStructRecord*>(h.value);
}
inline TimestampPoolRecord* Rec(TimestampPoolHandle h) {
  return reinterpret_cast<TimestampPoolRecord*>(h.value);
}
template <typename Handle, typename Record>
inline Handle MakeHandle(Record* record) {
  return Handle{reinterpret_cast<u64>(record)};
}
inline VkImageView View(TextureView v) { return reinterpret_cast<VkImageView>(v.value); }
inline TextureView MakeView(VkImageView view) {
  return TextureView{reinterpret_cast<u64>(view)};
}
inline VkSampler SamplerOf(SamplerHandle s) { return reinterpret_cast<VkSampler>(s.value); }

// --- conversion tables (vk_convert.cc) ---

VkFormat ToVkFormat(Format format);
Format FromVkFormat(VkFormat format);
VkImageAspectFlags AspectOf(Format format);
VkBufferUsageFlags ToVkBufferUsage(BufferUsageFlags usage);
VkImageUsageFlags ToVkImageUsage(TextureUsageFlags usage, Format format);
VkShaderStageFlags ToVkStages(ShaderStageFlags stages);
VkDescriptorType ToVkDescriptorType(BindingType type);
VkCompareOp ToVkCompare(CompareOp op);

struct StateInfo {
  VkImageLayout layout;
  VkPipelineStageFlags2 stages;
  VkAccessFlags2 access;
};
StateInfo StateInfoOf(ResourceState state, bool as_source);

struct ScopeInfo {
  VkPipelineStageFlags2 stages;
  VkAccessFlags2 access;
};
ScopeInfo ScopeInfoOf(BarrierScope scope, bool as_source);

class VulkanDevice;

class VulkanCommandList final : public CommandList {
 public:
  VulkanCommandList(VulkanDevice& device, VkCommandBuffer cmd, VkDescriptorPool transient_pool)
      : device_(device), cmd_(cmd), transient_pool_(transient_pool) {}

  void BindPipeline(PipelineHandle pipeline) override;
  void BindSet(u32 set_index, BindingSetHandle set) override;
  void BindTransient(u32 set_index, std::span<const BindingItem> items) override;
  void PushConstants(const void* data, u32 size, u32 offset) override;
  void Dispatch(u32 x, u32 y, u32 z) override;
  void BeginRendering(const RenderingInfo& info) override;
  void EndRendering() override;
  void SetViewport(f32 x, f32 y, f32 width, f32 height) override;
  void SetScissor(i32 x, i32 y, u32 width, u32 height) override;
  void BindVertexBuffer(u32 binding, const GpuBuffer& buffer, u64 offset) override;
  void BindIndexBuffer(const GpuBuffer& buffer, u64 offset, IndexType type) override;
  void Draw(u32 vertex_count, u32 instance_count, u32 first_vertex, u32 first_instance) override;
  void DrawIndexed(u32 index_count, u32 instance_count, u32 first_index, i32 vertex_offset,
                   u32 first_instance) override;
  void DrawIndexedIndirect(const GpuBuffer& args, u64 offset, u32 draw_count, u32 stride) override;
  void DrawMeshTasks(u32 x, u32 y, u32 z) override;
  void TextureBarriers(std::span<const TextureBarrier> barriers) override;
  void MemoryBarrier(BarrierScope src, BarrierScope dst) override;
  void CopyBufferToTexture(const GpuBuffer& src, const GpuImage& dst,
                           std::span<const BufferTextureCopy> regions) override;
  void CopyTextureToBuffer(const GpuImage& src, const GpuBuffer& dst,
                           const BufferTextureCopy& region) override;
  void CopyBuffer(const GpuBuffer& src, u64 src_offset, const GpuBuffer& dst, u64 dst_offset,
                  u64 size) override;
  void BlitMip(const GpuImage& image, u32 src_mip, Extent2D src_extent, u32 dst_mip,
               Extent2D dst_extent) override;
  void ClearColor(const GpuImage& image, const f32 color[4]) override;
  void ClearDepth(const GpuImage& image, f32 depth) override;
  void FillBuffer(const GpuBuffer& buffer, u64 offset, u64 size, u32 data) override;
  void BuildBlas(AccelStructHandle blas, const BlasBuildDesc& desc, const GpuBuffer& scratch,
                 u64 scratch_offset) override;
  void BuildTlas(AccelStructHandle tlas, const GpuBuffer& instances, u32 instance_count,
                 const GpuBuffer& scratch) override;
  void ResetTimestamps(TimestampPoolHandle pool, u32 first, u32 count) override;
  void WriteTimestamp(TimestampPoolHandle pool, u32 index, bool after_work) override;
  void BeginDebugLabel(const char* name) override;
  void EndDebugLabel() override;
  void* native_handle() override { return cmd_; }

  VkCommandBuffer cmd() const { return cmd_; }

 private:
  VulkanDevice& device_;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkDescriptorPool transient_pool_ = VK_NULL_HANDLE;
  const PipelineRecord* bound_ = nullptr;
};

class VulkanSwapchain final : public Swapchain {
 public:
  static std::unique_ptr<VulkanSwapchain> Create(VulkanDevice& device, u32 width, u32 height,
                                                 bool vsync);
  ~VulkanSwapchain() override;

  AcquireResult Acquire(u32 slot, u32* out_image_index) override;
  Format format() const override { return format_; }
  Extent2D extent() const override { return extent_; }
  u32 image_count() const override { return static_cast<u32>(images_.size()); }
  const GpuImage& image(u32 index) const override { return images_[index]; }
  bool can_sample() const override { return sampleable_; }

  VkSwapchainKHR swapchain() const { return swapchain_; }
  VkSemaphore render_finished(u32 image_index) const { return render_finished_[image_index]; }

 private:
  explicit VulkanSwapchain(VulkanDevice& device) : device_(device) {}
  bool Init(u32 width, u32 height, bool vsync);

  VulkanDevice& device_;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  Format format_ = Format::kUnknown;
  Extent2D extent_{};
  base::Vector<GpuImage> images_;
  base::Vector<TextureRecord> records_;
  // One per swapchain image: a present may still wait on the semaphore until
  // its image is reacquired, so frame slots cannot own these.
  base::Vector<VkSemaphore> render_finished_;
  bool sampleable_ = false;
};

class VulkanDevice final : public Device {
 public:
  static std::unique_ptr<Device> Create(const DeviceDesc& desc, Window& window);
  ~VulkanDevice() override;

  void WaitIdle() override;
  bool RecreateSurface(Window& window) override;
  void DestroySurface() override;
  std::unique_ptr<Swapchain> CreateSwapchain(u32 width, u32 height, bool vsync) override;
  MemoryBudget memory_budget() const override;

  GpuBuffer CreateBuffer(u64 size, BufferUsageFlags usage, bool host_visible) override;
  GpuBuffer CreateBufferWithData(ByteSpan data, BufferUsageFlags usage) override;
  void DestroyBuffer(GpuBuffer& buffer) override;
  GpuImage CreateImage2D(Format format, Extent2D extent, TextureUsageFlags usage,
                         u32 mip_levels) override;
  GpuImage CreateImageCube(Format format, u32 size, TextureUsageFlags usage,
                           u32 mip_levels) override;
  void DestroyImage(GpuImage& image) override;
  TextureView CreateMipView(const GpuImage& image, u32 mip) override;
  TextureView CreateArrayView(const GpuImage& image) override;
  void DestroyView(TextureView view) override;
  SamplerHandle GetSampler(const SamplerDesc& desc) override;

  PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) override;
  PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
  void DestroyPipeline(PipelineHandle pipeline) override;
  BindingLayoutHandle CreateBindingLayout(const BindingLayoutDesc& desc) override;
  void DestroyBindingLayout(BindingLayoutHandle layout) override;
  BindingSetHandle CreateBindingSet(BindingLayoutHandle layout, u32 variable_count) override;
  void DestroyBindingSet(BindingSetHandle set) override;
  void UpdateBindingSet(BindingSetHandle set, std::span<const BindingItem> items) override;

  AccelSizes GetBlasSizes(const BlasBuildDesc& desc) override;
  AccelSizes GetTlasSizes(u32 instance_count) override;
  AccelStructHandle CreateAccelStruct(AccelStructType type, u64 size) override;
  void DestroyAccelStruct(AccelStructHandle accel) override;
  u64 accel_address(AccelStructHandle accel) override;

  TimestampPoolHandle CreateTimestampPool(u32 count) override;
  void DestroyTimestampPool(TimestampPoolHandle pool) override;
  bool GetTimestamps(TimestampPoolHandle pool, u32 first, u32 count, u64* out) override;

  void ImmediateSubmit(const std::function<void(CommandList&)>& record) override;
  CommandList* BeginFrame(u32 slot) override;
  PresentResult SubmitFrame(CommandList* cmd, Swapchain& swapchain, u32 image_index) override;

  // Internal + interop surface.
  VkInstance instance() const { return instance_; }
  VkPhysicalDevice physical_device() const { return physical_device_; }
  VkDevice device() const { return device_; }
  VkSurfaceKHR surface() const { return surface_; }
  VkQueue graphics_queue() const { return graphics_queue_; }
  u32 graphics_family() const { return graphics_family_; }
  VmaAllocator allocator() const { return allocator_; }

  // Descriptor writes shared by BindTransient and UpdateBindingSet.
  void WriteDescriptors(VkDescriptorSet set, std::span<const BindingItem> items);
  VkPipelineLayout GetOrCreatePipelineLayout(std::span<const VkDescriptorSetLayout> sets,
                                             VkShaderStageFlags push_stages, u32 push_size);
  VkDescriptorSetLayout GetOrCreateSetLayout(const BindingLayoutDesc& desc);

 private:
  VulkanDevice() = default;

  bool InitResources();
  void ShutdownResources();
  GpuBuffer WrapBuffer(VkBuffer buffer, VmaAllocation allocation, u64 size, void* mapped,
                       u64 address);

  struct FrameRing {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore image_available = VK_NULL_HANDLE;
    VkFence in_flight = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    std::unique_ptr<VulkanCommandList> list;
    bool fence_armed = false;  // submit happened; next BeginFrame must wait
  };

  VkInstance instance_ = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue graphics_queue_ = VK_NULL_HANDLE;
  u32 graphics_family_ = 0;
  VmaAllocator allocator_ = nullptr;
  VkCommandPool immediate_pool_ = VK_NULL_HANDLE;
  VkFence immediate_fence_ = VK_NULL_HANDLE;
  VkDescriptorPool immediate_descriptor_pool_ = VK_NULL_HANDLE;
  FrameRing frames_[kMaxFramesInFlight];

  // Caches, keyed by content hash; entries live for the device's lifetime.
  base::UnorderedMap<u64, VkDescriptorSetLayout> set_layout_cache_;
  base::UnorderedMap<u64, VkPipelineLayout> pipeline_layout_cache_;
  base::UnorderedMap<u64, VkSampler> sampler_cache_;

  friend class VulkanSwapchain;
  friend class VulkanCommandList;
};

}  // namespace rec::render::vk

#endif  // RECREATION_RENDER_VULKAN_VK_BACKEND_H_
