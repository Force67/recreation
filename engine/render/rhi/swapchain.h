#ifndef RECREATION_RENDER_RHI_SWAPCHAIN_H_
#define RECREATION_RENDER_RHI_SWAPCHAIN_H_

#include <memory>

#include <base/containers/vector.h>

#include "render/rhi/device.h"

namespace rec::render {

class Swapchain {
 public:
  static std::unique_ptr<Swapchain> Create(Device& device, u32 width, u32 height, bool vsync);
  ~Swapchain();

  Swapchain(const Swapchain&) = delete;
  Swapchain& operator=(const Swapchain&) = delete;

  // VK_ERROR_OUT_OF_DATE_KHR and VK_SUBOPTIMAL_KHR pass through so the
  // renderer decides when to recreate.
  VkResult Acquire(VkSemaphore signal, u32* out_image_index);
  VkResult Present(VkSemaphore wait, u32 image_index);

  VkFormat format() const { return format_; }
  VkExtent2D extent() const { return extent_; }
  u32 image_count() const { return static_cast<u32>(images_.size()); }
  VkImage image(u32 index) const { return images_[index]; }
  VkImageView view(u32 index) const { return views_[index]; }

 private:
  explicit Swapchain(Device& device) : device_(device) {}

  bool Init(u32 width, u32 height, bool vsync);

  Device& device_;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkFormat format_ = VK_FORMAT_UNDEFINED;
  VkExtent2D extent_{};
  base::Vector<VkImage> images_;
  base::Vector<VkImageView> views_;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_RHI_SWAPCHAIN_H_
