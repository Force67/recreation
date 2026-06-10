#include "core/window.h"

#include "core/log.h"

namespace rec {
namespace {

// Fallback when SDL3 is not compiled in or window creation fails. Keeps the
// main loop and headless server running everywhere.
class HeadlessWindow final : public Window {
 public:
  explicit HeadlessWindow(const WindowDesc& desc) : width_(desc.width), height_(desc.height) {}

  bool PumpEvents() override { return true; }
  NativeWindowHandles native_handles() const override { return {}; }
  u32 width() const override { return width_; }
  u32 height() const override { return height_; }

 private:
  u32 width_;
  u32 height_;
};

}  // namespace

#if defined(RECREATION_HAS_SDL3)
std::unique_ptr<Window> CreateSdl3Window(const WindowDesc& desc);
#endif

std::unique_ptr<Window> Window::Create(const WindowDesc& desc) {
#if defined(RECREATION_HAS_SDL3)
  if (auto window = CreateSdl3Window(desc)) return window;
#endif
  REC_WARN("no window backend available, running headless");
  return std::make_unique<HeadlessWindow>(desc);
}

}  // namespace rec
