// Recreation's ultragui scripting backend. Injected as ULTRAGUI_SCRIPT_SOURCE so
// it replaces libultragui's bundled Lua runtime (and the no-op stub): ugui's
// interaction layer -- button clicks, on_change handlers, ugui.find/set from a
// handler -- is driven by recreation's hosted .NET world instead of Lua.
//
// The library stays oblivious to C#. This file speaks only the host-agnostic seam
// in ugui_csharp_host.h: it forwards each ugui handler to a dispatch callback the
// host installs, and exposes a WidgetOps table the host calls back through. The
// recreation runtime is what bolts the dispatch pointer onto the managed (C#)
// host; from libultragui's side this is just "some host".
//
// One ScriptRuntime exists per UIContext, and ugui resolves widgets against the
// thread's active registry (WidgetRegistry::Active()), so the name->handle map
// and the installed dispatch live in process-global state shared by the instance
// methods and the free-function WidgetOps. That matches how the Lua backend
// leans on the active registry.

#include "ugui_csharp_host.h"

#include <ugui/scripting/script_runtime.h>
#include <ugui/style/enums.h>
#include <ugui/widgets/button.h>
#include <ugui/widgets/checkbox.h>
#include <ugui/widgets/dropdown.h>
#include <ugui/widgets/slider.h>
#include <ugui/widgets/text.h>
#include <ugui/widgets/widget.h>
#include <ugui/widgets/widget_registry.h>

#include <cstring>
#include <string>
#include <unordered_map>

namespace ugui {
namespace {

// Pack/unpack a WidgetId <-> u64 for the host boundary (0 == null widget).
inline std::uint64_t Pack(wid w) {
  return (static_cast<std::uint64_t>(w.generation) << 32) | w.index;
}
inline wid Unpack(std::uint64_t v) {
  return wid{static_cast<u32>(v & 0xffffffffu), static_cast<u32>(v >> 32)};
}

// Process-global name->widget registry, shared by the (single) ScriptRuntime and
// the WidgetOps free functions so a C# handler's ugui.find resolves the same set
// the runtime registered on load.
std::unordered_map<std::string, wid>& Registry() {
  static std::unordered_map<std::string, wid> registry;
  return registry;
}

// The host dispatch callback installed via the seam. Null until the host wires it
// (e.g. a dedicated-server / no-managed build leaves handlers inert).
rx::ugui_cs::DispatchFn g_dispatch = nullptr;
void* g_dispatch_ctx = nullptr;

// Resolve a packed handle to its live node, or null if stale / unknown.
WidgetNode* NodeOf(std::uint64_t widget) {
  wid w = Unpack(widget);
  if (!w.valid()) return nullptr;
  World& world = *WidgetRegistry::Active();
  return world.Get<WidgetNode>(w);
}

}  // namespace

// --- WidgetOps implementation (backend -> host call-backs) ------------------

namespace {

std::uint64_t OpFind(const char* name) {
  if (!name) return 0;
  auto it = Registry().find(name);
  return it != Registry().end() ? Pack(it->second) : 0;
}

std::int32_t OpGetText(std::uint64_t widget, char* buf, std::int32_t buf_len) {
  WidgetNode* n = NodeOf(widget);
  if (!n) return 0;
  World& world = *WidgetRegistry::Active();
  wid w = Unpack(widget);
  const String* text = nullptr;
  if (n->kind == WidgetKind::kText) {
    if (auto* c = world.Get<TextContent>(w)) text = &c->text;
  } else if (n->kind == WidgetKind::kButton) {
    if (auto* c = world.Get<ButtonContent>(w)) text = &c->label;
  }
  if (!text) return 0;
  auto full = static_cast<std::int32_t>(text->size());
  if (buf && buf_len > 0) {
    std::int32_t copy = full < buf_len - 1 ? full : buf_len - 1;
    std::memcpy(buf, text->data(), static_cast<size_t>(copy));
    buf[copy] = '\0';
  }
  return full;
}

void OpSetText(std::uint64_t widget, const char* text) {
  WidgetNode* n = NodeOf(widget);
  if (!n || !text) return;
  wid w = Unpack(widget);
  World& world = *WidgetRegistry::Active();
  if (n->kind == WidgetKind::kText) {
    SetText(w, text);
    ClearAnimationStyle(world, w);
  } else if (n->kind == WidgetKind::kButton) {
    SetButtonLabel(w, text);
    ClearAnimationStyle(world, w);
  }
}

std::int32_t OpGetChecked(std::uint64_t widget) {
  WidgetNode* n = NodeOf(widget);
  if (!n || n->kind != WidgetKind::kCheckbox) return 0;
  return IsChecked(Unpack(widget)) ? 1 : 0;
}

void OpSetChecked(std::uint64_t widget, std::int32_t checked) {
  WidgetNode* n = NodeOf(widget);
  if (n && n->kind == WidgetKind::kCheckbox) SetChecked(Unpack(widget), checked != 0);
}

float OpGetValue(std::uint64_t widget) {
  WidgetNode* n = NodeOf(widget);
  if (!n || n->kind != WidgetKind::kSlider) return 0.0f;
  return SliderValue(Unpack(widget));
}

void OpSetValue(std::uint64_t widget, float value) {
  WidgetNode* n = NodeOf(widget);
  if (n && n->kind == WidgetKind::kSlider) SetSliderValue(Unpack(widget), value);
}

std::int32_t OpGetSelected(std::uint64_t widget) {
  WidgetNode* n = NodeOf(widget);
  if (!n || n->kind != WidgetKind::kDropdown) return -1;
  return DropdownSelected(Unpack(widget));
}

void OpSetSelected(std::uint64_t widget, std::int32_t index) {
  WidgetNode* n = NodeOf(widget);
  if (n && n->kind == WidgetKind::kDropdown) SetDropdownSelected(Unpack(widget), index);
}

std::int32_t OpGetVisible(std::uint64_t widget) {
  wid w = Unpack(widget);
  if (!w.valid()) return 0;
  World& world = *WidgetRegistry::Active();
  auto* sc = world.Get<StyleC>(w);
  return (sc && sc->style.visibility == Visibility::kVisible) ? 1 : 0;
}

void OpSetVisible(std::uint64_t widget, std::int32_t visible) {
  wid w = Unpack(widget);
  if (!w.valid()) return;
  World& world = *WidgetRegistry::Active();
  auto* sc = world.Get<StyleC>(w);
  if (!sc) return;
  Style s = sc->style;
  s.visibility = visible ? Visibility::kVisible : Visibility::kHidden;
  SetStyle(world, w, s);
  ClearAnimationStyle(world, w);
}

constexpr rx::ugui_cs::WidgetOps kWidgetOps = {
    OpFind,     OpGetText,    OpSetText,     OpGetChecked,  OpSetChecked, OpGetValue,
    OpSetValue, OpGetSelected, OpSetSelected, OpGetVisible, OpSetVisible,
};

}  // namespace

// --- ScriptRuntime: the swappable runtime ultragui drives -------------------

struct ScriptRuntime::Impl {};

ScriptRuntime::ScriptRuntime() : impl_(nullptr) {}
ScriptRuntime::~ScriptRuntime() {}

bool ScriptRuntime::Init() { return true; }

void ScriptRuntime::Shutdown() { Registry().clear(); }

// C# UI logic lives in compiled assemblies the managed host loads, not in script
// chunks ugui hands us, so there is nothing to interpret here.
bool ScriptRuntime::Exec(const char*, const char*) { return false; }
bool ScriptRuntime::ExecFile(const char*) { return false; }

void ScriptRuntime::RegisterWidget(wid widget) {
  World& world = *WidgetRegistry::Active();
  WidgetNode* n = world.Get<WidgetNode>(widget);
  if (n && !n->name.empty()) Registry()[n->name] = widget;
}

void ScriptRuntime::UnregisterWidget(wid widget) {
  World& world = *WidgetRegistry::Active();
  WidgetNode* n = world.Get<WidgetNode>(widget);
  if (n && !n->name.empty()) Registry().erase(n->name);
}

void ScriptRuntime::ClearWidgetRegistry() { Registry().clear(); }

wid ScriptRuntime::FindRegisteredWidget(const char* name) const {
  if (!name) return kNullWidget;
  auto it = Registry().find(name);
  return it != Registry().end() ? it->second : kNullWidget;
}

// The crossing point: a ugui handler fired, route it to the host (the managed
// world). Returns whether the host claimed it.
bool ScriptRuntime::CallHandler(const char* func_name, wid widget) {
  if (!g_dispatch || !func_name) return false;
  return g_dispatch(g_dispatch_ctx, func_name, Pack(widget)) != 0;
}

// Mirror the Lua backend: give every named dropdown/checkbox/slider an on_change
// that dispatches on_<name>(widget) into the host.
static void WireChangeHandlersRecursive(ScriptRuntime& rt, wid w) {
  if (!w.valid()) return;
  World& world = *WidgetRegistry::Active();
  WidgetNode* n = world.Get<WidgetNode>(w);
  if (!n) return;
  if (!n->name.empty()) {
    auto dispatch = [&rt, w]() {
      World& wr = *WidgetRegistry::Active();
      WidgetNode* node = wr.Get<WidgetNode>(w);
      if (!node) return;
      std::string handler = "on_" + node->name;
      rt.CallHandler(handler.c_str(), w);
    };
    if (n->kind == WidgetKind::kDropdown)
      SetDropdownChange(w, [dispatch](i32, const String&) { dispatch(); });
    else if (n->kind == WidgetKind::kCheckbox)
      SetCheckboxChange(w, [dispatch](bool) { dispatch(); });
    else if (n->kind == WidgetKind::kSlider)
      SetSliderChange(w, [dispatch](f32) { dispatch(); });
  }
  for (wid child : world.Get<Hierarchy>(w)->children) WireChangeHandlersRecursive(rt, child);
}

void ScriptRuntime::WireChangeHandlers(wid root) { WireChangeHandlersRecursive(*this, root); }

// Timers/tweens are owned by the managed world (ticked from its frame callback),
// so the native side has nothing to advance.
void ScriptRuntime::ScheduleTimer(double, int) {}
void ScriptRuntime::SyncTimerClock(double) {}
void ScriptRuntime::ClearTimersAndTweens() {}
void ScriptRuntime::UpdateTimers(double) {}

}  // namespace ugui

// --- Host seam (defined here, called by the recreation runtime) -------------

namespace rx::ugui_cs {

void InstallHostDispatch(DispatchFn dispatch, void* ctx) {
  ugui::g_dispatch = dispatch;
  ugui::g_dispatch_ctx = ctx;
}

const WidgetOps* GetWidgetOps() { return &ugui::kWidgetOps; }

}  // namespace rx::ugui_cs
