#include <SDL3/SDL.h>

#include "core/log.h"
#include "core/window.h"

namespace rec {
namespace {

class Sdl3Window final : public Window {
 public:
  explicit Sdl3Window(SDL_Window* window) : window_(window) {}

  ~Sdl3Window() override {
    SDL_DestroyWindow(window_);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
  }

  bool PumpEvents() override {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_EVENT_QUIT) return false;
    }
    return true;
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
