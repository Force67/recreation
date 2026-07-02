#include "render/rhi/vulkan_interop.h"

#include "render/vulkan/vk_backend.h"

namespace rec::render {

VulkanHandles GetVulkanHandles(Device& device) {
  if (device.caps().backend != Backend::kVulkan) return {};
  auto& vk_device = static_cast<vk::VulkanDevice&>(device);
  return {.instance = vk_device.instance(),
          .physical_device = vk_device.physical_device(),
          .device = vk_device.device(),
          .graphics_queue = vk_device.graphics_queue(),
          .graphics_family = vk_device.graphics_family(),
          .allocator = vk_device.allocator()};
}

VkImage GetVkImage(const GpuImage& image) {
  return image.handle ? vk::Rec(image.handle)->image : VK_NULL_HANDLE;
}

VkImageView GetVkImageView(TextureView view) { return vk::View(view); }

VkSampler GetVkSampler(SamplerHandle sampler) { return vk::SamplerOf(sampler); }

VkBuffer GetVkBuffer(const GpuBuffer& buffer) {
  return buffer.handle ? vk::Rec(buffer.handle)->buffer : VK_NULL_HANDLE;
}

VkAccelerationStructureKHR GetVkAccelStruct(AccelStructHandle accel) {
  return accel ? vk::Rec(accel)->accel : VK_NULL_HANDLE;
}

VkFormat GetVkFormat(Format format) { return vk::ToVkFormat(format); }

}  // namespace rec::render
