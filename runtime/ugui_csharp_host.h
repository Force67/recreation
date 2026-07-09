#ifndef RECREATION_RUNTIME_UGUI_CSHARP_HOST_H_
#define RECREATION_RUNTIME_UGUI_CSHARP_HOST_H_

#include <cstdint>

// The seam between recreation's ultragui scripting backend (ugui_script_csharp.cc,
// compiled into libultragui as ULTRAGUI_SCRIPT_SOURCE) and whatever host wants to
// drive ugui's interaction layer. It is deliberately host-agnostic: libultragui
// only ever calls a function pointer the host installs and exposes a plain table
// of widget operations. That C# happens to sit on the other side of the dispatch
// pointer is a recreation implementation detail the UI library never sees.
//
// Two directions cross here:
//   * host -> backend: InstallHostDispatch() hands the backend the callback it
//     fires whenever a ugui handler runs (a button click, an on_change). Until a
//     host installs one, handler invocations are silently dropped.
//   * backend -> host: GetWidgetOps() returns the table the host (or the managed
//     world behind it) calls back through to read and mutate live widgets, so a
//     handler can flip a checkbox, retitle a button, hide a panel, etc.
//
// A widget is addressed as a u64: the 8-byte ugui::WidgetId packed as
// (generation << 32 | index). 0 is the null widget.

namespace rx::ugui_cs {

// Read/write operations on live ugui widgets, resolved against the thread's
// active widget registry (the one UIContext makes current). Plain C function
// pointers so a managed mirror can call straight through; never null individually.
// Append only -- the managed side mirrors this layout field for field.
struct WidgetOps {
  // Resolve a registered widget name to its packed handle (0 if unknown).
  std::uint64_t (*find)(const char* name);

  // Text / button label. get writes UTF-8 (truncated) into buf and returns the
  // full length; set replaces the text/label of a kText/kButton widget.
  std::int32_t (*get_text)(std::uint64_t widget, char* buf, std::int32_t buf_len);
  void (*set_text)(std::uint64_t widget, const char* text);

  // Checkbox state (1/0). No-op / 0 on a non-checkbox.
  std::int32_t (*get_checked)(std::uint64_t widget);
  void (*set_checked)(std::uint64_t widget, std::int32_t checked);

  // Slider value. 0 / no-op on a non-slider.
  float (*get_value)(std::uint64_t widget);
  void (*set_value)(std::uint64_t widget, float value);

  // Dropdown selected index. -1 / no-op on a non-dropdown.
  std::int32_t (*get_selected)(std::uint64_t widget);
  void (*set_selected)(std::uint64_t widget, std::int32_t index);

  // Visibility (1 = visible). Toggling re-styles the widget immediately.
  std::int32_t (*get_visible)(std::uint64_t widget);
  void (*set_visible)(std::uint64_t widget, std::int32_t visible);
};

// The callback the backend fires for every ugui handler. ctx is the opaque host
// pointer passed to InstallHostDispatch. Returns 1 if the host handled the call.
using DispatchFn = std::int32_t (*)(void* ctx, const char* func_name, std::uint64_t widget);

// Install (or clear, with a null dispatch) the host handler-dispatch callback.
// Defined in the backend; called by the host once the managed world is up.
void InstallHostDispatch(DispatchFn dispatch, void* ctx);

// The backend's widget operation table, stable for the process lifetime. The
// host hands this to the managed world so C# handlers can manipulate widgets.
// Defined in the backend; never null.
const WidgetOps* GetWidgetOps();

}  // namespace rx::ugui_cs

#endif  // RECREATION_RUNTIME_UGUI_CSHARP_HOST_H_
