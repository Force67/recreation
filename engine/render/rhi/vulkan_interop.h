#ifndef RECREATION_RENDER_RHI_VULKAN_INTEROP_H_
#define RECREATION_RENDER_RHI_VULKAN_INTEROP_H_

// Vulkan escape hatch for modules that integrate api-specific SDKs (NRD, DLSS,
// FSR3, the imgui/ugui gui backend, thumbnailer). Everything here returns null
// handles when the device is not the Vulkan backend, so callers must feature-
// gate on GetVulkanHandles(...).device != VK_NULL_HANDLE. Pass code must NOT
// include this header; it exists so interop stays possible without leaking
// Vulkan back into the renderer's portable surface.

#include <volk.h>

#include <vk_mem_alloc.h>

#include "render/rhi/command_list.h"
#include "render/rhi/device.h"

namespace rec::render {

struct VulkanHandles {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue graphics_queue = VK_NULL_HANDLE;
  u32 graphics_family = 0;
  VmaAllocator allocator = nullptr;
};

// Null-filled when `device` is not the Vulkan backend.
VulkanHandles GetVulkanHandles(Device& device);

// The command list's VkCommandBuffer, or null on other backends.
inline VkCommandBuffer GetVkCommandBuffer(CommandList& cmd) {
  return static_cast<VkCommandBuffer>(cmd.native_handle());
}

// Raw handles behind rhi resources, for handing to api-specific SDKs.
VkImage GetVkImage(const GpuImage& image);
VkImageView GetVkImageView(TextureView view);
VkSampler GetVkSampler(SamplerHandle sampler);
VkBuffer GetVkBuffer(const GpuBuffer& buffer);
VkAccelerationStructureKHR GetVkAccelStruct(AccelStructHandle accel);
VkFormat GetVkFormat(Format format);

}  // namespace rec::render

#endif  // RECREATION_RENDER_RHI_VULKAN_INTEROP_H_
