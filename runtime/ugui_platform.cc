// Custom ugui::Platform for the recreation engine. Compiled into the ultragui
// static library in place of the bundled GLFW backend (ULTRAGUI_PLATFORM_SOURCE)
// so the library needs no windowing dependency of its own. The engine owns the
// SDL3 window and feeds this shim through a UguiHostState pointer passed as
// UIConfig::external_window, plus events pushed into input_queue().

#include <ugui/platform/platform.h>

#include <chrono>

#include "ugui_platform.h"

namespace ugui {

struct Platform::Impl {
  rx::ui::UguiHostState* host = nullptr;
  InputQueue queue;
  std::chrono::steady_clock::time_point start{};
};

Platform::Platform() : impl_(new Impl()) {}
Platform::~Platform() { delete impl_; }

bool Platform::Init(const WindowConfig& config) {
  impl_->host = static_cast<rx::ui::UguiHostState*>(config.external_window);
  impl_->start = std::chrono::steady_clock::now();
  return true;
}

void Platform::Shutdown() { impl_->host = nullptr; }

bool Platform::ShouldClose() const { return impl_->host && impl_->host->should_close; }

void Platform::PollEvents() {
  // No-op: the engine drains the OS event queue itself and pushes the relevant
  // events into input_queue() before each RenderDrawData(). InputRouter::Process
  // consumes and clears the queue.
}

Vec2 Platform::window_size() const {
  if (!impl_->host) return {1280.0f, 720.0f};
  return {impl_->host->window_width, impl_->host->window_height};
}

Vec2 Platform::framebuffer_size() const { return window_size(); }

f32 Platform::dpi_scale() const { return impl_->host ? impl_->host->dpi_scale : 1.0f; }

f64 Platform::time() const {
  auto now = std::chrono::steady_clock::now();
  return std::chrono::duration<f64>(now - impl_->start).count();
}

void* Platform::native_handle() const { return impl_->host; }

void Platform::SetCursor(Cursor) {}

const char* Platform::clipboard_text() const { return ""; }
void Platform::set_clipboard_text(const char*) {}

InputQueue& Platform::input_queue() { return impl_->queue; }

}  // namespace ugui
