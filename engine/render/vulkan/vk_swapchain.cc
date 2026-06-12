#include <algorithm>

#include "core/log.h"
#include "render/rhi/swapchain.h"

namespace rec::render {

std::unique_ptr<Swapchain> Swapchain::Create(Device& device, u32 width, u32 height, bool vsync) {
  auto swapchain = std::unique_ptr<Swapchain>(new Swapchain(device));
  if (!swapchain->Init(width, height, vsync)) return nullptr;
  return swapchain;
}

bool Swapchain::Init(u32 width, u32 height, bool vsync) {
  VkSurfaceCapabilitiesKHR caps;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_.physical_device(), device_.surface(), &caps);

  u32 format_count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(device_.physical_device(), device_.surface(), &format_count,
                                       nullptr);
  base::Vector<VkSurfaceFormatKHR> formats(format_count);
  vkGetPhysicalDeviceSurfaceFormatsKHR(device_.physical_device(), device_.surface(), &format_count,
                                       formats.data());

  // UNORM, not SRGB: tonemapping and the output transfer function are the
  // engine's job at the end of the post stack.
  VkSurfaceFormatKHR chosen = formats[0];
  for (const auto& format : formats) {
    if (format.format == VK_FORMAT_B8G8R8A8_UNORM &&
        format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      chosen = format;
      break;
    }
  }
  format_ = chosen.format;

  u32 mode_count = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(device_.physical_device(), device_.surface(),
                                            &mode_count, nullptr);
  base::Vector<VkPresentModeKHR> modes(mode_count);
  vkGetPhysicalDeviceSurfacePresentModesKHR(device_.physical_device(), device_.surface(),
                                            &mode_count, modes.data());
  // Fifo is the vsync path and always available; mailbox gives unthrottled
  // frames without tearing when vsync is off.
  VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
  if (!vsync && modes.find(VK_PRESENT_MODE_MAILBOX_KHR) != nullptr) {
    present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
  }

  if (caps.currentExtent.width != 0xffffffff) {
    extent_ = caps.currentExtent;
  } else {
    extent_.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent_.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
  }

  u32 image_count = caps.minImageCount + 1;
  if (caps.maxImageCount > 0) image_count = std::min(image_count, caps.maxImageCount);

  VkSwapchainCreateInfoKHR info{.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  info.surface = device_.surface();
  info.minImageCount = image_count;
  info.imageFormat = format_;
  info.imageColorSpace = chosen.colorSpace;
  info.imageExtent = extent_;
  info.imageArrayLayers = 1;
  info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.preTransform = caps.currentTransform;
  info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  info.presentMode = present_mode;
  info.clipped = VK_TRUE;

  if (vkCreateSwapchainKHR(device_.device(), &info, nullptr, &swapchain_) != VK_SUCCESS) {
    REC_ERROR("swapchain creation failed");
    return false;
  }

  u32 count = 0;
  vkGetSwapchainImagesKHR(device_.device(), swapchain_, &count, nullptr);
  images_.resize(count);
  vkGetSwapchainImagesKHR(device_.device(), swapchain_, &count, images_.data());

  views_.resize(count);
  for (u32 i = 0; i < count; ++i) {
    VkImageViewCreateInfo view_info{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = images_[i];
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format_;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_.device(), &view_info, nullptr, &views_[i]) != VK_SUCCESS) {
      REC_ERROR("swapchain image view creation failed");
      return false;
    }
  }

  REC_INFO("swapchain: {}x{}, {} images, {}", extent_.width, extent_.height, count,
           present_mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo");
  return true;
}

Swapchain::~Swapchain() {
  for (VkImageView view : views_) vkDestroyImageView(device_.device(), view, nullptr);
  if (swapchain_ != VK_NULL_HANDLE) vkDestroySwapchainKHR(device_.device(), swapchain_, nullptr);
}

VkResult Swapchain::Acquire(VkSemaphore signal, u32* out_image_index) {
  return vkAcquireNextImageKHR(device_.device(), swapchain_, UINT64_MAX, signal, VK_NULL_HANDLE,
                               out_image_index);
}

VkResult Swapchain::Present(VkSemaphore wait, u32 image_index) {
  VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  info.waitSemaphoreCount = 1;
  info.pWaitSemaphores = &wait;
  info.swapchainCount = 1;
  info.pSwapchains = &swapchain_;
  info.pImageIndices = &image_index;
  return vkQueuePresentKHR(device_.graphics_queue(), &info);
}

}  // namespace rec::render
