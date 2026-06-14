#ifndef RECREATION_RENDER_RHI_DEVICE_H_
#define RECREATION_RENDER_RHI_DEVICE_H_

#include <volk.h>

#include <functional>
#include <memory>
#include <string>

#include "core/types.h"
#include "core/window.h"
#include "render/rhi/resources.h"

namespace rec::render {

struct DeviceDesc {
  bool enable_validation = false;
  bool request_raytracing = true;
};

// What the picked GPU actually supports. Optional features are queried,
// never assumed, so the same binary runs on a desktop GPU and an android
// phone. Vulkan 1.3 with dynamic rendering, synchronization2, buffer device
// address, descriptor indexing and timeline semaphores is the hard baseline.
struct DeviceCaps {
  std::string adapter_name;
  u32 api_version = 0;
  bool raytracing = false;  // acceleration structures + ray tracing pipeline
  bool ray_query = false;
  bool mesh_shaders = false;
  bool fragment_shading_rate = false;
  bool fill_mode_non_solid = false;  // wireframe debug views
  f32 max_anisotropy = 1.0f;         // 1 = anisotropic filtering unavailable
  f32 timestamp_period = 0.0f;       // ns per timestamp tick, 0 = no gpu timing
  bool debug_utils = false;          // VK_EXT_debug_utils for markers/labels
  bool integrated = false;           // integrated/handheld gpu (shared memory)
  u64 device_local_bytes = 0;        // summed DEVICE_LOCAL heap size, vram proxy
};

// Owns instance, surface, physical and logical device and the queues.
// Returned device is a stub (is_stub() true) when no loader, no capable GPU
// or no presentable window is available.
class Device {
 public:
  static std::unique_ptr<Device> Create(const DeviceDesc& desc, Window& window);
  ~Device();

  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  const DeviceCaps& caps() const { return caps_; }
  bool is_stub() const { return device_ == VK_NULL_HANDLE; }

  VkInstance instance() const { return instance_; }
  VkPhysicalDevice physical_device() const { return physical_device_; }
  VkDevice device() const { return device_; }
  VkSurfaceKHR surface() const { return surface_; }
  VkQueue graphics_queue() const { return graphics_queue_; }
  u32 graphics_family() const { return graphics_family_; }
  VmaAllocator allocator() const { return allocator_; }

  void WaitIdle();

  // Live gpu memory usage from VMA, summed over the device-local heaps, for the
  // debug overlay. budget is the driver's estimate of what the app may use.
  struct MemoryBudget {
    u64 used_bytes = 0;
    u64 budget_bytes = 0;
    u64 allocated_bytes = 0;  // bytes in live VMA allocations
    u32 allocation_count = 0;
  };
  MemoryBudget memory_budget() const;

  // Records into a transient command buffer and blocks until execution
  // finished. For uploads and one-off transitions, not the frame path.
  void ImmediateSubmit(const std::function<void(VkCommandBuffer)>& record);

  GpuBuffer CreateBuffer(u64 size, VkBufferUsageFlags usage, bool host_visible = false);
  GpuBuffer CreateBufferWithData(ByteSpan data, VkBufferUsageFlags usage);
  void DestroyBuffer(GpuBuffer& buffer);

  GpuImage CreateImage2D(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage,
                         VkImageAspectFlags aspect, u32 mip_levels = 1);
  GpuImage CreateImageCube(VkFormat format, u32 size, VkImageUsageFlags usage,
                           u32 mip_levels = 1);
  void DestroyImage(GpuImage& image);

 private:
  Device() = default;

  bool InitResources();
  void ShutdownResources();

  DeviceCaps caps_;
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
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_RHI_DEVICE_H_
