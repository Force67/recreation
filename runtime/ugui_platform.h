#ifndef RECREATION_RUNTIME_UGUI_PLATFORM_H_
#define RECREATION_RUNTIME_UGUI_PLATFORM_H_

// Bridge between the engine and the custom ugui::Platform backend that is
// compiled into the ultragui static library (ugui_platform.cc, wired through
// ULTRAGUI_PLATFORM_SOURCE). ultragui runs in draw-data mode here: it owns no
// window. The engine hands a pointer to one of these as
// UIConfig::external_window; the platform reads viewport/close state from it
// and the engine pushes input straight into platform()->input_queue().

namespace rx::ui {

struct UguiHostState {
  float window_width = 1280.0f;
  float window_height = 720.0f;
  float dpi_scale = 1.0f;
  bool should_close = false;
};

}  // namespace rx::ui

#endif  // RECREATION_RUNTIME_UGUI_PLATFORM_H_
