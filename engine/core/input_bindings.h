#ifndef RECREATION_CORE_INPUT_BINDINGS_H_
#define RECREATION_CORE_INPUT_BINDINGS_H_

#include <string>
#include <vector>

#include "core/input.h"
#include "core/input_actions.h"
#include "core/types.h"

namespace rec {

// A single physical source that drives an action or axis. For kGamepadAxis,
// axis_dir picks a half-axis when the binding feeds a digital Action (+1 = the
// positive half, e.g. stick-down/right past the deadzone), or 0 when it feeds an
// analog Axis with its full signed value.
enum class SourceKind : u8 { kNone, kKey, kMouseButton, kGamepadButton, kGamepadAxis };

struct Binding {
  SourceKind kind = SourceKind::kNone;
  u16 code = 0;     // Key / MouseButton / GamepadButton / GamepadAxis as integer
  i8 axis_dir = 0;  // gamepad-axis half-axis sign for digital use; 0 for analog

  bool operator==(const Binding& o) const {
    return kind == o.kind && code == o.code && axis_dir == o.axis_dir;
  }
};

// Owns the action/axis binding tables and the look/deadzone options, resolves
// raw input into an ActionState each pump, and serialises to a controls INI.
// One instance lives for the session; Resolve keeps the previous held state so
// it can report per-action press edges regardless of the source device.
class InputMap {
 public:
  static constexpr int kMaxBindings = 3;  // per action/axis (primary + alternates)

  InputMap() { LoadDefaults(); }

  // Installs the built-in keyboard/mouse + gamepad layout.
  void LoadDefaults();

  // Builds `out` from this pump's raw input. Non-const: tracks press edges and
  // the last active device across calls.
  void Resolve(const InputState& kbm, const GamepadState& pad, ActionState* out);

  // --- Rebinding (used by the settings UI) ---
  const std::vector<Binding>& bindings(Action a) const { return action_[idx(a)]; }
  const std::vector<Binding>& bindings(Axis a) const { return axis_[idx(a)]; }
  void ClearBindings(Action a) { action_[idx(a)].clear(); }
  void ClearBindings(Axis a) { axis_[idx(a)].clear(); }
  // Adds a binding (capped at kMaxBindings, drops duplicates).
  void AddBinding(Action a, Binding b);
  void AddBinding(Axis a, Binding b);
  // Rebinds an action to `b`: removes `b` from every other action (no dup),
  // replaces `a`'s existing same-family binding (keyboard/mouse vs gamepad), and
  // keeps the other-device binding. Used by the settings capture flow.
  void Rebind(Action a, Binding b);
  // Restores the built-in bindings and the look/deadzone options.
  void ResetToDefaults();
  // Returns the action this source is already bound to, or Action::kCount if
  // free. Lets the UI warn about / clear conflicts before committing a rebind.
  Action ConflictingAction(const Binding& b) const;

  // --- Options (persisted) ---
  f32 look_sens_kbm = 0.0025f;  // radians per mouse pixel
  f32 look_sens_pad = 2.6f;     // radians per second at full stick deflection
  bool invert_y = false;
  f32 stick_deadzone = 0.15f;
  f32 trigger_threshold = 0.5f;  // pull past this to fire a digital action
  bool rumble = true;
  bool adaptive_triggers = true;
  u8 led_r = 0, led_g = 60, led_b = 120;  // DualSense lightbar

  // --- Persistence ---
  bool LoadFromIni(const std::string& path);
  bool SaveToIni(const std::string& path) const;

  // Default controls path: $XDG_CONFIG_HOME/recreation/controls.ini (or the
  // platform equivalent). Empty if no home directory can be found.
  static std::string DefaultConfigPath();

 private:
  static int idx(Action a) { return static_cast<int>(a); }
  static int idx(Axis a) { return static_cast<int>(a); }

  bool SourceHeld(const Binding& b, const InputState& kbm, const GamepadState& pad) const;
  f32 AxisValue(const Binding& b, const GamepadState& pad) const;

  std::vector<Binding> action_[static_cast<int>(Action::kCount)];
  std::vector<Binding> axis_[static_cast<int>(Axis::kCount)];
  bool prev_held_[static_cast<int>(Action::kCount)] = {};
  InputDevice last_device_ = InputDevice::kKeyboardMouse;
};

// Human-readable names for the INI and the rebind UI. Round-trip safe.
const char* ActionName(Action a);
bool ActionFromName(const char* name, Action* out);
const char* AxisName(Axis a);
bool AxisFromName(const char* name, Axis* out);
// Source token, e.g. "key:W", "mouse:left", "pad:south", "padaxis:lefty+".
std::string BindingToken(const Binding& b);
bool BindingFromToken(const char* token, Binding* out);
// Short label for a binding as shown next to an action in the settings list.
std::string BindingLabel(const Binding& b);

}  // namespace rec

#endif  // RECREATION_CORE_INPUT_BINDINGS_H_
