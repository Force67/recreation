#ifndef RECREATION_CORE_WINDOW_H_
#define RECREATION_CORE_WINDOW_H_

#include <memory>
#include <string>
#include <vector>

#include "core/types.h"

namespace rec {

struct WindowDesc {
  std::string title = "recreation";
  u32 width = 1920;
  u32 height = 1080;
  bool fullscreen = false;
};

// Opaque handles the renderer needs to create a surface. With the SDL3
// backend `window` is the SDL_Window, headless leaves both null.
struct NativeWindowHandles {
  void* window = nullptr;
  void* display = nullptr;
};

class Window {
 public:
  virtual ~Window() = default;

  virtual bool PumpEvents() = 0;
  virtual NativeWindowHandles native_handles() const = 0;
  virtual u32 width() const = 0;
  virtual u32 height() const = 0;

  // Vulkan glue. Backends that can present return the instance extensions
  // they need and write a VkSurfaceKHR through the opaque out pointer.
  // Headless windows return nothing, which tells the renderer to stay off.
  virtual std::vector<const char*> vulkan_instance_extensions() const { return {}; }
  virtual bool CreateVulkanSurface(void* vk_instance, void* out_vk_surface) { return false; }

  // Returns a platform window, or a headless stub when none is available.
  static std::unique_ptr<Window> Create(const WindowDesc& desc);
};

}  // namespace rec

#endif  // RECREATION_CORE_WINDOW_H_
