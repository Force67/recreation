#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <cstring>

#include "core/log.h"
#include "core/window.h"

namespace rec {
namespace {

Key TranslateKey(SDL_Scancode code) {
  switch (code) {
    case SDL_SCANCODE_W: return Key::kW;
    case SDL_SCANCODE_A: return Key::kA;
    case SDL_SCANCODE_S: return Key::kS;
    case SDL_SCANCODE_D: return Key::kD;
    case SDL_SCANCODE_Q: return Key::kQ;
    case SDL_SCANCODE_E: return Key::kE;
    case SDL_SCANCODE_F: return Key::kF;
    case SDL_SCANCODE_T: return Key::kT;
    case SDL_SCANCODE_C: return Key::kC;
    case SDL_SCANCODE_SPACE: return Key::kSpace;
    case SDL_SCANCODE_LSHIFT: return Key::kLeftShift;
    case SDL_SCANCODE_LCTRL: return Key::kLeftCtrl;
    case SDL_SCANCODE_ESCAPE: return Key::kEscape;
    case SDL_SCANCODE_F1: return Key::kF1;
    case SDL_SCANCODE_F2: return Key::kF2;
    case SDL_SCANCODE_F3: return Key::kF3;
    case SDL_SCANCODE_1: return Key::k1;
    case SDL_SCANCODE_2: return Key::k2;
    case SDL_SCANCODE_3: return Key::k3;
    case SDL_SCANCODE_4: return Key::k4;
    default: return Key::kCount;
  }
}

MouseButton TranslateButton(u8 button) {
  switch (button) {
    case SDL_BUTTON_LEFT: return MouseButton::kLeft;
    case SDL_BUTTON_RIGHT: return MouseButton::kRight;
    case SDL_BUTTON_MIDDLE: return MouseButton::kMiddle;
    default: return MouseButton::kCount;
  }
}

class Sdl3Window final : public Window {
 public:
  explicit Sdl3Window(SDL_Window* window) : window_(window) {}

  ~Sdl3Window() override {
    SDL_DestroyWindow(window_);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
  }

  bool PumpEvents() override {
    input_.mouse_dx = 0;
    input_.mouse_dy = 0;
    input_.wheel = 0;
    std::memset(input_.pressed, 0, sizeof(input_.pressed));

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event_hook_) event_hook_(&event);
      switch (event.type) {
        case SDL_EVENT_QUIT:
          return false;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
          Key key = TranslateKey(event.key.scancode);
          if (key == Key::kCount) break;
          bool down = event.type == SDL_EVENT_KEY_DOWN;
          if (down && !event.key.repeat && !input_.keys[static_cast<u8>(key)]) {
            input_.pressed[static_cast<u8>(key)] = true;
          }
          input_.keys[static_cast<u8>(key)] = down;
          break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
          input_.mouse_x = event.button.x;
          input_.mouse_y = event.button.y;
          MouseButton button = TranslateButton(event.button.button);
          if (button == MouseButton::kCount) break;
          input_.mouse[static_cast<u8>(button)] = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
          break;
        }
        case SDL_EVENT_MOUSE_MOTION:
          input_.mouse_dx += event.motion.xrel;
          input_.mouse_dy += event.motion.yrel;
          input_.mouse_x = event.motion.x;
          input_.mouse_y = event.motion.y;
          break;
        case SDL_EVENT_MOUSE_WHEEL:
          input_.wheel += event.wheel.y;
          break;
        default:
          break;
      }
    }
    return true;
  }

  void SetRelativeMouseMode(bool enabled) override {
    SDL_SetWindowRelativeMouseMode(window_, enabled);
  }

  bool relative_mouse_mode() const override {
    return SDL_GetWindowRelativeMouseMode(window_);
  }

  NativeWindowHandles native_handles() const override {
    // The renderer goes through SDL_Vulkan_CreateSurface, which takes the
    // SDL_Window itself rather than platform handles.
    return {window_, nullptr};
  }

  u32 width() const override {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    return static_cast<u32>(w);
  }

  u32 height() const override {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    return static_cast<u32>(h);
  }

  std::vector<const char*> vulkan_instance_extensions() const override {
    Uint32 count = 0;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!extensions) return {};
    return {extensions, extensions + count};
  }

  bool CreateVulkanSurface(void* vk_instance, void* out_vk_surface) override {
    return SDL_Vulkan_CreateSurface(window_, static_cast<VkInstance>(vk_instance), nullptr,
                                    static_cast<VkSurfaceKHR*>(out_vk_surface));
  }

 private:
  SDL_Window* window_;
};

}  // namespace

std::unique_ptr<Window> CreateSdl3Window(const WindowDesc& desc) {
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
    REC_ERROR("sdl init failed: {}", SDL_GetError());
    return nullptr;
  }
  SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
  if (desc.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
  SDL_Window* window = SDL_CreateWindow(desc.title.c_str(), static_cast<int>(desc.width),
                                        static_cast<int>(desc.height), flags);
  if (!window) {
    REC_ERROR("sdl window creation failed: {}", SDL_GetError());
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return nullptr;
  }
  return std::make_unique<Sdl3Window>(window);
}

}  // namespace rec
