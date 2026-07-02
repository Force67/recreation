#include <algorithm>

#include "core/log.h"
#include "render/vulkan/vk_backend.h"

namespace rec::render::vk {

std::unique_ptr<VulkanSwapchain> VulkanSwapchain::Create(VulkanDevice& device, u32 width,
                                                         u32 height, bool vsync, bool hdr) {
  auto swapchain = std::unique_ptr<VulkanSwapchain>(new VulkanSwapchain(device));
  if (!swapchain->Init(width, height, vsync, hdr)) return nullptr;
  return swapchain;
}

bool VulkanSwapchain::Init(u32 width, u32 height, bool vsync, bool hdr) {
  VkSurfaceCapabilitiesKHR caps;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_.physical_device(), device_.surface(), &caps);

  u32 format_count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(device_.physical_device(), device_.surface(), &format_count,
                                       nullptr);
  base::Vector<VkSurfaceFormatKHR> formats(format_count);
  vkGetPhysicalDeviceSurfaceFormatsKHR(device_.physical_device(), device_.surface(), &format_count,
                                       formats.data());

  // UNORM, not SRGB: tonemapping and the output transfer function are the
  // engine's job at the end of the post stack. With `hdr` the HDR10 PQ
  // 10-bit format is preferred (widest display support), then fp16 scRGB;
  // when the surface offers neither (X11 compositors), SDR silently wins and
  // color_space() reports it so the tonemap keeps encoding sRGB.
  VkSurfaceFormatKHR chosen = formats[0];
  color_space_ = ColorSpace::kSrgbNonlinear;
  bool found = false;
  if (hdr) {
    for (const auto& format : formats) {
      if (format.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
          format.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
        chosen = format;
        color_space_ = ColorSpace::kHdr10Pq;
        found = true;
        break;
      }
    }
    if (!found) {
      for (const auto& format : formats) {
        if (format.format == VK_FORMAT_R16G16B16A16_SFLOAT &&
            format.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) {
          chosen = format;
          color_space_ = ColorSpace::kScRgbLinear;
          found = true;
          break;
        }
      }
    }
    if (!found) REC_WARN("hdr swapchain requested but the surface only offers sdr");
  }
  if (!found) {
    for (const auto& format : formats) {
      if (format.format == VK_FORMAT_B8G8R8A8_UNORM &&
          format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        chosen = format;
        break;
      }
    }
  }
  format_ = FromVkFormat(chosen.format);
  if (format_ == Format::kUnknown) {
    REC_ERROR("surface format {} has no rhi mapping", static_cast<int>(chosen.format));
    return false;
  }

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
    extent_ = {caps.currentExtent.width, caps.currentExtent.height};
  } else {
    extent_.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent_.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
  }

  // +2 over the minimum: frame generation acquires two images per engine frame
  // (interpolated + real); the extra slack keeps the second acquire's
  // forward-progress guarantee (outstanding acquires <= count - min).
  u32 image_count = caps.minImageCount + 2;
  if (caps.maxImageCount > 0) image_count = std::min(image_count, caps.maxImageCount);

  // Desktop surfaces support OPAQUE; Android surfaces often only offer INHERIT.
  // Pick the first supported value rather than assuming OPAQUE.
  VkCompositeAlphaFlagBitsKHR composite = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  for (VkCompositeAlphaFlagBitsKHR pref :
       {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
    if (caps.supportedCompositeAlpha & pref) {
      composite = pref;
      break;
    }
  }

  // On a portrait-native panel the surface reports a rotated currentTransform.
  // Letting the display engine apply it (preTransform = identity) keeps the
  // engine rendering in the surface's reported orientation with no pre-rotation
  // pass; fall back to the current transform when identity is unsupported.
  VkSurfaceTransformFlagBitsKHR transform = caps.currentTransform;
  if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) {
    transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  }

  VkSwapchainCreateInfoKHR info{.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  info.surface = device_.surface();
  info.minImageCount = image_count;
  info.imageFormat = chosen.format;
  info.imageColorSpace = chosen.colorSpace;
  info.imageExtent = {extent_.width, extent_.height};
  info.imageArrayLayers = 1;
  info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  // Sampling the backbuffer (UI backdrop blur) needs SAMPLED; add it only when
  // the surface supports it so swapchain creation never fails for the feature.
  if (caps.supportedUsageFlags & VK_IMAGE_USAGE_SAMPLED_BIT) {
    info.imageUsage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    sampleable_ = true;
  }
  info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.preTransform = transform;
  info.compositeAlpha = composite;
  info.presentMode = present_mode;
  info.clipped = VK_TRUE;

  if (vkCreateSwapchainKHR(device_.device(), &info, nullptr, &swapchain_) != VK_SUCCESS) {
    REC_ERROR("swapchain creation failed");
    return false;
  }

  u32 count = 0;
  vkGetSwapchainImagesKHR(device_.device(), swapchain_, &count, nullptr);
  base::Vector<VkImage> vk_images(count);
  vkGetSwapchainImagesKHR(device_.device(), swapchain_, &count, vk_images.data());

  records_.resize(count);
  images_.resize(count);
  render_finished_.resize(count);
  for (u32 i = 0; i < count; ++i) {
    VkImageViewCreateInfo view_info{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = vk_images[i];
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = chosen.format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(device_.device(), &view_info, nullptr, &view) != VK_SUCCESS) {
      REC_ERROR("swapchain image view creation failed");
      return false;
    }
    records_[i] = {.image = vk_images[i], .view = view, .format = chosen.format};
    images_[i] = {.handle = MakeHandle<TextureHandle>(&records_[i]),
                  .view = MakeView(view),
                  .format = format_,
                  .extent = extent_};

    VkSemaphoreCreateInfo semaphore_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(device_.device(), &semaphore_info, nullptr, &render_finished_[i]);
  }

  const char* space = color_space_ == ColorSpace::kHdr10Pq       ? ", hdr10 pq"
                      : color_space_ == ColorSpace::kScRgbLinear ? ", scrgb"
                                                                 : "";
  REC_INFO("swapchain: {}x{}, {} images, {}{}", extent_.width, extent_.height, count,
           present_mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo", space);
  return true;
}

VulkanSwapchain::~VulkanSwapchain() {
  for (VkSemaphore semaphore : render_finished_) {
    if (semaphore) vkDestroySemaphore(device_.device(), semaphore, nullptr);
  }
  for (TextureRecord& record : records_) {
    if (record.view) vkDestroyImageView(device_.device(), record.view, nullptr);
  }
  if (swapchain_ != VK_NULL_HANDLE) vkDestroySwapchainKHR(device_.device(), swapchain_, nullptr);
}

AcquireResult VulkanSwapchain::Acquire(u32 slot, u32* out_image_index) {
  VkResult result = vkAcquireNextImageKHR(device_.device(), swapchain_, UINT64_MAX,
                                          device_.frames_[slot].image_available, VK_NULL_HANDLE,
                                          out_image_index);
  switch (result) {
    case VK_SUCCESS: return AcquireResult::kOk;
    case VK_SUBOPTIMAL_KHR: return AcquireResult::kSuboptimal;
    case VK_ERROR_OUT_OF_DATE_KHR: return AcquireResult::kOutOfDate;
    default: return AcquireResult::kFailed;
  }
}

AcquireResult VulkanSwapchain::AcquireSecond(u32 slot, u32* out_image_index) {
  VkResult result = vkAcquireNextImageKHR(device_.device(), swapchain_, UINT64_MAX,
                                          device_.frames_[slot].image_available_fg,
                                          VK_NULL_HANDLE, out_image_index);
  switch (result) {
    case VK_SUCCESS: return AcquireResult::kOk;
    case VK_SUBOPTIMAL_KHR: return AcquireResult::kSuboptimal;
    case VK_ERROR_OUT_OF_DATE_KHR: return AcquireResult::kOutOfDate;
    default: return AcquireResult::kFailed;
  }
}

}  // namespace rec::render::vk
