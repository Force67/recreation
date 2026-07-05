#ifndef RECREATION_CORE_WINDOW_H_
#define RECREATION_CORE_WINDOW_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/input.h"
#include "core/types.h"

#if defined(__ANDROID__)
struct ANativeWindow;
#endif

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

  // Input collected by the last PumpEvents.
  const InputState& input() const { return input_; }
  const GamepadState& gamepad() const { return gamepad_; }

  // Gamepad haptics. No-ops unless a pad is connected; the DualSense-only
  // effects (trigger resistance, lightbar) silently do nothing on other pads,
  // so callers can issue them unconditionally.
  virtual void SetRumble(f32 low_freq, f32 high_freq, u32 duration_ms) {}
  virtual void SetTriggerEffect(bool left, bool right, const TriggerEffect& effect) {}
  virtual void SetLedColor(u8 r, u8 g, u8 b) {}

  // While enabled the cursor is hidden and mouse_dx/dy keep accumulating
  // without hitting the screen edge. Mouse look uses this.
  virtual void SetRelativeMouseMode(bool enabled) {}
  virtual bool relative_mouse_mode() const { return false; }

  // True when the OS/compositor is actually presenting this window with HDR
  // headroom (Windows advanced-color toggle on, Wayland color-management HDR,
  // macOS EDR) - NOT merely an HDR-capable display. The renderer gates its HDR
  // swapchain request on this: a Vulkan surface can advertise HDR10 formats
  // while the system toggle is off, and presenting PQ then washes out. Can flip
  // at runtime (OS setting, window moved between monitors); polled per frame.
  virtual bool hdr_enabled() const { return false; }

  // Called for every native event before the window handles it. With the
  // SDL3 backend the pointer is an SDL_Event. ImGui hooks in here.
  void set_event_hook(std::function<void(const void* native_event)> hook) {
    event_hook_ = std::move(hook);
  }

  // Vulkan glue. Backends that can present return the instance extensions
  // they need and write a VkSurfaceKHR through the opaque out pointer.
  // Headless windows return nothing, which tells the renderer to stay off.
  virtual std::vector<const char*> vulkan_instance_extensions() const { return {}; }
  virtual bool CreateVulkanSurface(void* vk_instance, void* out_vk_surface) { return false; }

  // Returns a platform window, or a headless stub when none is available.
  static std::unique_ptr<Window> Create(const WindowDesc& desc);

 protected:
  InputState input_;
  GamepadState gamepad_;
  std::function<void(const void*)> event_hook_;
};

#if defined(__ANDROID__)
// Android window the activity drives. The activity owns the native-glue event
// loop and feeds input and lifecycle into the window each frame, so the engine
// keeps its own Run()/RunFrame() structure unchanged.
class AndroidWindowBase : public Window {
 public:
  virtual InputState& mutable_input() = 0;
  virtual void RequestQuit() = 0;
  virtual ::ANativeWindow* native_window() const = 0;
  // Rebinds to a new ANativeWindow across the activity lifecycle (the old one
  // is released, the new one acquired). null marks the window as gone.
  virtual void SetNativeWindow(::ANativeWindow* window) = 0;
};

std::unique_ptr<AndroidWindowBase> CreateAndroidWindow(::ANativeWindow* window);
#endif

}  // namespace rec

#endif  // RECREATION_CORE_WINDOW_H_
