#include "core/window.h"

#include "core/log.h"

namespace rec {
namespace {

// Placeholder until the per platform backends (win32, wayland/x11, android)
// land. Lets the runtime loop and headless server run everywhere today.
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

std::unique_ptr<Window> Window::Create(const WindowDesc& desc) {
  REC_WARN("no native window backend yet, running headless");
  return std::make_unique<HeadlessWindow>(desc);
}

}  // namespace rec
