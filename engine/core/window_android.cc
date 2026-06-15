#include <android/native_window.h>

#include <volk.h>

#include "core/log.h"
#include "core/window.h"

namespace rec {
namespace {

// Wraps the ANativeWindow the activity hands us. Unlike the SDL3 backend this
// owns no event loop: the activity (runtime/android_main.cc) pumps the native
// glue and drives the engine a frame at a time, feeding input through
// mutable_input(). The window only provides the Vulkan surface and the
// backbuffer extent.
class AndroidWindow final : public AndroidWindowBase {
 public:
  explicit AndroidWindow(ANativeWindow* window) : window_(window) {}

  ~AndroidWindow() override {
    if (window_) ANativeWindow_release(window_);
  }

  bool PumpEvents() override { return !quit_; }

  NativeWindowHandles native_handles() const override { return {window_, nullptr}; }

  u32 width() const override {
    return window_ ? static_cast<u32>(ANativeWindow_getWidth(window_)) : 0;
  }
  u32 height() const override {
    return window_ ? static_cast<u32>(ANativeWindow_getHeight(window_)) : 0;
  }

  std::vector<const char*> vulkan_instance_extensions() const override {
    return {"VK_KHR_surface", "VK_KHR_android_surface"};
  }

  bool CreateVulkanSurface(void* vk_instance, void* out_vk_surface) override {
    if (!window_) return false;
    if (vkCreateAndroidSurfaceKHR == nullptr) {
      REC_ERROR("vkCreateAndroidSurfaceKHR not loaded (VK_KHR_android_surface not enabled)");
      return false;
    }
    VkAndroidSurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    info.window = window_;
    VkResult result =
        vkCreateAndroidSurfaceKHR(static_cast<VkInstance>(vk_instance), &info, nullptr,
                                  static_cast<VkSurfaceKHR*>(out_vk_surface));
    if (result != VK_SUCCESS) {
      REC_ERROR("vkCreateAndroidSurfaceKHR failed: {}", static_cast<int>(result));
      return false;
    }
    return true;
  }

  // AndroidWindowBase: the activity feeds input and lifecycle here.
  InputState& mutable_input() override { return input_; }
  void RequestQuit() override { quit_ = true; }
  ANativeWindow* native_window() const override { return window_; }

 private:
  ANativeWindow* window_;
  bool quit_ = false;
};

}  // namespace

std::unique_ptr<AndroidWindowBase> CreateAndroidWindow(ANativeWindow* window) {
  return std::make_unique<AndroidWindow>(window);
}

}  // namespace rec
