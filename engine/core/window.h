#ifndef RECREATION_CORE_WINDOW_H_
#define RECREATION_CORE_WINDOW_H_

#include <memory>
#include <string>

#include "core/types.h"

namespace rec {

struct WindowDesc {
  std::string title = "recreation";
  u32 width = 1920;
  u32 height = 1080;
  bool fullscreen = false;
};

// Native handles the renderer needs to create a surface. Exactly one pair is
// valid depending on platform (hwnd/hinstance, wl_surface/wl_display, ANativeWindow).
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

  // Returns a platform window, or a headless stub when none is available.
  static std::unique_ptr<Window> Create(const WindowDesc& desc);
};

}  // namespace rec

#endif  // RECREATION_CORE_WINDOW_H_
