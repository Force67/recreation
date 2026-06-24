#include "core/input_bindings.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace rec {
namespace {

// --- Name tables. Each is indexed by the matching enum value, so order here
// must track the enum declarations. ---

// Bindable keys, in Key declaration order (input.h).
constexpr const char* kKeyTokens[] = {
    "W",      "A",      "S",         "D",     "Q",      "E",     "F",     "T",
    "C",      "R",      "G",         "X",     "Z",      "B",     "V",     "Space",
    "LShift", "LCtrl",  "Escape",    "F1",    "F2",     "F3",    "F4",    "F5",
    "Delete", "Backspace", "Return", "1",     "2",      "3",     "4",     "J",
    "Up",     "Down",   "Left",      "Right", "Tab",
};
static_assert(sizeof(kKeyTokens) / sizeof(kKeyTokens[0]) == static_cast<int>(Key::kCount),
              "kKeyTokens must match the Key enum");

constexpr const char* kMouseTokens[] = {"left", "right", "middle"};
static_assert(sizeof(kMouseTokens) / sizeof(kMouseTokens[0]) ==
                  static_cast<int>(MouseButton::kCount),
              "kMouseTokens must match the MouseButton enum");

constexpr const char* kPadButtonTokens[] = {
    "south",  "east",      "west",      "north",   "back",    "guide",
    "start",  "lstick",    "rstick",    "lshoulder", "rshoulder",
    "dpup",   "dpdown",    "dpleft",    "dpright", "touchpad",
};
static_assert(sizeof(kPadButtonTokens) / sizeof(kPadButtonTokens[0]) ==
                  static_cast<int>(GamepadButton::kCount),
              "kPadButtonTokens must match the GamepadButton enum");

constexpr const char* kPadAxisTokens[] = {"leftx",    "lefty",    "rightx",
                                          "righty",   "ltrigger", "rtrigger"};
static_assert(sizeof(kPadAxisTokens) / sizeof(kPadAxisTokens[0]) ==
                  static_cast<int>(GamepadAxis::kCount),
              "kPadAxisTokens must match the GamepadAxis enum");

// Action / Axis INI names, in enum order.
constexpr const char* kActionNames[] = {
    "move_forward",    "move_back",      "move_left",      "move_right",
    "jump",            "sprint",         "sneak",          "cam_up",
    "cam_down",        "activate",       "attack",         "ready",
    "throw_debug",     "toggle_walk",    "toggle_third_person", "toggle_journal",
    "toggle_editor",   "toggle_menu",    "toggle_debug",   "toggle_trace",
    "toggle_quests",   "menu_up",        "menu_down",      "menu_left",
    "menu_right",      "menu_accept",    "menu_cancel",    "menu_tab",
    "menu_page_left",  "menu_page_right",
};
static_assert(sizeof(kActionNames) / sizeof(kActionNames[0]) == static_cast<int>(Action::kCount),
              "kActionNames must match the Action enum");

constexpr const char* kAxisNames[] = {"move_x", "move_y", "look_x", "look_y"};
static_assert(sizeof(kAxisNames) / sizeof(kAxisNames[0]) == static_cast<int>(Axis::kCount),
              "kAxisNames must match the Axis enum");

int FindToken(const char* name, const char* const* table, int count) {
  for (int i = 0; i < count; ++i)
    if (std::strcmp(name, table[i]) == 0) return i;
  return -1;
}

}  // namespace

// --- Name lookups -----------------------------------------------------------

const char* ActionName(Action a) { return kActionNames[static_cast<int>(a)]; }
const char* AxisName(Axis a) { return kAxisNames[static_cast<int>(a)]; }

bool ActionFromName(const char* name, Action* out) {
  int i = FindToken(name, kActionNames, static_cast<int>(Action::kCount));
  if (i < 0) return false;
  *out = static_cast<Action>(i);
  return true;
}

bool AxisFromName(const char* name, Axis* out) {
  int i = FindToken(name, kAxisNames, static_cast<int>(Axis::kCount));
  if (i < 0) return false;
  *out = static_cast<Axis>(i);
  return true;
}

std::string BindingToken(const Binding& b) {
  switch (b.kind) {
    case SourceKind::kKey:
      if (b.code < static_cast<u16>(Key::kCount)) return std::string("key:") + kKeyTokens[b.code];
      break;
    case SourceKind::kMouseButton:
      if (b.code < static_cast<u16>(MouseButton::kCount))
        return std::string("mouse:") + kMouseTokens[b.code];
      break;
    case SourceKind::kGamepadButton:
      if (b.code < static_cast<u16>(GamepadButton::kCount))
        return std::string("pad:") + kPadButtonTokens[b.code];
      break;
    case SourceKind::kGamepadAxis:
      if (b.code < static_cast<u16>(GamepadAxis::kCount)) {
        std::string s = std::string("padaxis:") + kPadAxisTokens[b.code];
        if (b.axis_dir > 0) s += '+';
        else if (b.axis_dir < 0) s += '-';
        return s;
      }
      break;
    case SourceKind::kNone:
      break;
  }
  return "";
}

bool BindingFromToken(const char* token, Binding* out) {
  const char* colon = std::strchr(token, ':');
  if (!colon) return false;
  std::string kind(token, colon - token);
  std::string rest(colon + 1);
  Binding b;
  if (kind == "key") {
    int i = FindToken(rest.c_str(), kKeyTokens, static_cast<int>(Key::kCount));
    if (i < 0) return false;
    b.kind = SourceKind::kKey;
    b.code = static_cast<u16>(i);
  } else if (kind == "mouse") {
    int i = FindToken(rest.c_str(), kMouseTokens, static_cast<int>(MouseButton::kCount));
    if (i < 0) return false;
    b.kind = SourceKind::kMouseButton;
    b.code = static_cast<u16>(i);
  } else if (kind == "pad") {
    int i = FindToken(rest.c_str(), kPadButtonTokens, static_cast<int>(GamepadButton::kCount));
    if (i < 0) return false;
    b.kind = SourceKind::kGamepadButton;
    b.code = static_cast<u16>(i);
  } else if (kind == "padaxis") {
    i8 dir = 0;
    if (!rest.empty() && (rest.back() == '+' || rest.back() == '-')) {
      dir = rest.back() == '+' ? 1 : -1;
      rest.pop_back();
    }
    int i = FindToken(rest.c_str(), kPadAxisTokens, static_cast<int>(GamepadAxis::kCount));
    if (i < 0) return false;
    b.kind = SourceKind::kGamepadAxis;
    b.code = static_cast<u16>(i);
    b.axis_dir = dir;
  } else {
    return false;
  }
  *out = b;
  return true;
}

std::string BindingLabel(const Binding& b) {
  switch (b.kind) {
    case SourceKind::kKey:
      return b.code < static_cast<u16>(Key::kCount) ? kKeyTokens[b.code] : "?";
    case SourceKind::kMouseButton:
      return b.code < static_cast<u16>(MouseButton::kCount)
                 ? std::string("Mouse ") + kMouseTokens[b.code]
                 : "?";
    case SourceKind::kGamepadButton:
      return b.code < static_cast<u16>(GamepadButton::kCount)
                 ? std::string("Pad ") + kPadButtonTokens[b.code]
                 : "?";
    case SourceKind::kGamepadAxis: {
      if (b.code >= static_cast<u16>(GamepadAxis::kCount)) return "?";
      std::string s = std::string("Pad ") + kPadAxisTokens[b.code];
      if (b.axis_dir > 0) s += "+";
      else if (b.axis_dir < 0) s += "-";
      return s;
    }
    case SourceKind::kNone:
      break;
  }
  return "(unbound)";
}

// --- Defaults ---------------------------------------------------------------

void InputMap::LoadDefaults() {
  for (auto& v : action_) v.clear();
  for (auto& v : axis_) v.clear();

  auto key = [](Key k) { return Binding{SourceKind::kKey, static_cast<u16>(k), 0}; };
  auto mb = [](MouseButton m) { return Binding{SourceKind::kMouseButton, static_cast<u16>(m), 0}; };
  auto pad = [](GamepadButton g) {
    return Binding{SourceKind::kGamepadButton, static_cast<u16>(g), 0};
  };
  auto axis = [](GamepadAxis a, i8 dir) {
    return Binding{SourceKind::kGamepadAxis, static_cast<u16>(a), dir};
  };

  // Movement (keyboard discrete; the sticks feed the analog Axis values below).
  AddBinding(Action::kMoveForward, key(Key::kW));
  AddBinding(Action::kMoveBack, key(Key::kS));
  AddBinding(Action::kMoveLeft, key(Key::kA));
  AddBinding(Action::kMoveRight, key(Key::kD));
  AddBinding(Action::kJump, key(Key::kSpace));
  AddBinding(Action::kJump, pad(GamepadButton::kSouth));
  AddBinding(Action::kSprint, key(Key::kLeftShift));
  AddBinding(Action::kSprint, pad(GamepadButton::kLeftStick));
  AddBinding(Action::kSneak, key(Key::kLeftCtrl));
  AddBinding(Action::kSneak, pad(GamepadButton::kRightStick));
  AddBinding(Action::kCamUp, key(Key::kE));
  AddBinding(Action::kCamUp, pad(GamepadButton::kRightShoulder));
  AddBinding(Action::kCamDown, key(Key::kQ));
  AddBinding(Action::kCamDown, pad(GamepadButton::kLeftShoulder));

  // Gameplay verbs.
  AddBinding(Action::kActivate, key(Key::kE));
  AddBinding(Action::kActivate, pad(GamepadButton::kWest));
  AddBinding(Action::kAttack, mb(MouseButton::kLeft));
  AddBinding(Action::kAttack, axis(GamepadAxis::kRightTrigger, 1));
  AddBinding(Action::kReady, key(Key::kR));
  AddBinding(Action::kReady, pad(GamepadButton::kNorth));
  AddBinding(Action::kThrowDebug, key(Key::kF));

  // Mode toggles.
  AddBinding(Action::kToggleWalk, key(Key::kT));
  AddBinding(Action::kToggleThirdPerson, key(Key::kC));
  AddBinding(Action::kToggleJournal, key(Key::kJ));
  AddBinding(Action::kToggleJournal, pad(GamepadButton::kTouchpad));
  AddBinding(Action::kToggleEditor, key(Key::kF4));
  AddBinding(Action::kToggleMenu, key(Key::kEscape));
  AddBinding(Action::kToggleMenu, pad(GamepadButton::kStart));
  AddBinding(Action::kToggleDebug, key(Key::kF1));
  AddBinding(Action::kToggleTrace, key(Key::kF2));
  AddBinding(Action::kToggleQuests, key(Key::kF3));

  // Menu navigation: keyboard arrows + gamepad dpad and left stick.
  AddBinding(Action::kMenuUp, key(Key::kArrowUp));
  AddBinding(Action::kMenuUp, pad(GamepadButton::kDpadUp));
  AddBinding(Action::kMenuUp, axis(GamepadAxis::kLeftY, -1));
  AddBinding(Action::kMenuDown, key(Key::kArrowDown));
  AddBinding(Action::kMenuDown, pad(GamepadButton::kDpadDown));
  AddBinding(Action::kMenuDown, axis(GamepadAxis::kLeftY, 1));
  AddBinding(Action::kMenuLeft, key(Key::kArrowLeft));
  AddBinding(Action::kMenuLeft, pad(GamepadButton::kDpadLeft));
  AddBinding(Action::kMenuLeft, axis(GamepadAxis::kLeftX, -1));
  AddBinding(Action::kMenuRight, key(Key::kArrowRight));
  AddBinding(Action::kMenuRight, pad(GamepadButton::kDpadRight));
  AddBinding(Action::kMenuRight, axis(GamepadAxis::kLeftX, 1));
  AddBinding(Action::kMenuAccept, key(Key::kReturn));
  AddBinding(Action::kMenuAccept, pad(GamepadButton::kSouth));
  AddBinding(Action::kMenuCancel, key(Key::kEscape));
  AddBinding(Action::kMenuCancel, pad(GamepadButton::kEast));
  AddBinding(Action::kMenuTab, key(Key::kTab));
  AddBinding(Action::kMenuPageLeft, pad(GamepadButton::kLeftShoulder));
  AddBinding(Action::kMenuPageRight, pad(GamepadButton::kRightShoulder));

  // Analog axes: left stick drives movement, right stick drives the look.
  AddBinding(Axis::kMoveX, axis(GamepadAxis::kLeftX, 0));
  AddBinding(Axis::kMoveY, axis(GamepadAxis::kLeftY, 0));
  AddBinding(Axis::kLookX, axis(GamepadAxis::kRightX, 0));
  AddBinding(Axis::kLookY, axis(GamepadAxis::kRightY, 0));
}

void InputMap::AddBinding(Action a, Binding b) {
  auto& v = action_[idx(a)];
  if (static_cast<int>(v.size()) >= kMaxBindings) return;
  if (std::find(v.begin(), v.end(), b) == v.end()) v.push_back(b);
}

void InputMap::AddBinding(Axis a, Binding b) {
  auto& v = axis_[idx(a)];
  if (static_cast<int>(v.size()) >= kMaxBindings) return;
  if (std::find(v.begin(), v.end(), b) == v.end()) v.push_back(b);
}

namespace {
// Keyboard and mouse share a "family"; gamepad button/axis share the other. A
// rebind replaces only the same-family binding so the cross-device pairing
// (e.g. keyboard + pad) survives.
int SourceFamily(SourceKind k) {
  return (k == SourceKind::kKey || k == SourceKind::kMouseButton) ? 0 : 1;
}
}  // namespace

void InputMap::Rebind(Action a, Binding b) {
  // Remove this source from every action so it isn't bound twice.
  for (auto& v : action_)
    v.erase(std::remove(v.begin(), v.end(), b), v.end());
  // Drop the target's existing same-family binding, then add the new one.
  auto& t = action_[idx(a)];
  const int fam = SourceFamily(b.kind);
  t.erase(std::remove_if(t.begin(), t.end(),
                         [&](const Binding& x) { return SourceFamily(x.kind) == fam; }),
          t.end());
  if (static_cast<int>(t.size()) < kMaxBindings) t.push_back(b);
  else t.back() = b;
}

void InputMap::ResetToDefaults() {
  LoadDefaults();
  look_sens_kbm = 0.0025f;
  look_sens_pad = 2.6f;
  invert_y = false;
  stick_deadzone = 0.15f;
  trigger_threshold = 0.5f;
  rumble = true;
  adaptive_triggers = true;
  led_r = 0;
  led_g = 60;
  led_b = 120;
}

Action InputMap::ConflictingAction(const Binding& b) const {
  for (int a = 0; a < static_cast<int>(Action::kCount); ++a)
    if (std::find(action_[a].begin(), action_[a].end(), b) != action_[a].end())
      return static_cast<Action>(a);
  return Action::kCount;
}

// --- Resolution -------------------------------------------------------------

bool InputMap::SourceHeld(const Binding& b, const InputState& kbm, const GamepadState& pad) const {
  switch (b.kind) {
    case SourceKind::kKey:
      return b.code < static_cast<u16>(Key::kCount) && kbm.keys[b.code];
    case SourceKind::kMouseButton:
      return b.code < static_cast<u16>(MouseButton::kCount) && kbm.mouse[b.code];
    case SourceKind::kGamepadButton:
      return pad.connected && b.code < static_cast<u16>(GamepadButton::kCount) &&
             pad.buttons[b.code];
    case SourceKind::kGamepadAxis: {
      if (!pad.connected || b.code >= static_cast<u16>(GamepadAxis::kCount)) return false;
      f32 v = pad.axes[b.code];
      bool trigger = b.code == static_cast<u16>(GamepadAxis::kLeftTrigger) ||
                     b.code == static_cast<u16>(GamepadAxis::kRightTrigger);
      f32 thr = trigger ? trigger_threshold : stick_deadzone;
      if (b.axis_dir > 0) return v > thr;
      if (b.axis_dir < 0) return v < -thr;
      return std::fabs(v) > thr;
    }
    case SourceKind::kNone:
      return false;
  }
  return false;
}

f32 InputMap::AxisValue(const Binding& b, const GamepadState& pad) const {
  if (b.kind != SourceKind::kGamepadAxis || !pad.connected ||
      b.code >= static_cast<u16>(GamepadAxis::kCount))
    return 0;
  f32 v = pad.axes[b.code];
  if (std::fabs(v) < stick_deadzone) return 0;
  // Rescale so the value ramps from 0 at the deadzone edge to 1 at full throw.
  f32 s = (std::fabs(v) - stick_deadzone) / (1.0f - stick_deadzone);
  s = std::clamp(s, 0.0f, 1.0f);
  return std::copysign(s, v);
}

void InputMap::Resolve(const InputState& kbm, const GamepadState& pad, ActionState* out) {
  // Analog axes from their gamepad sources (summed, then clamped).
  for (int a = 0; a < static_cast<int>(Axis::kCount); ++a) {
    f32 v = 0;
    for (const Binding& b : axis_[a]) v += AxisValue(b, pad);
    out->analog[a] = std::clamp(v, -1.0f, 1.0f);
  }

  // Digital actions: a source held => action held; edge from the previous pump.
  for (int a = 0; a < static_cast<int>(Action::kCount); ++a) {
    bool held = false;
    for (const Binding& b : action_[a])
      if (SourceHeld(b, kbm, pad)) {
        held = true;
        break;
      }
    out->held[a] = held;
    out->edge[a] = held && !prev_held_[a];
    prev_held_[a] = held;
  }

  // Fold keyboard movement into the move axes so gameplay reads one value.
  // Stick convention is SDL's: down/right positive, so forward subtracts on Y.
  auto fold = [&](Axis ax, Action positive, Action negative) {
    f32 v = out->analog[idx(ax)];
    if (out->down(positive)) v += 1.0f;
    if (out->down(negative)) v -= 1.0f;
    out->analog[idx(ax)] = std::clamp(v, -1.0f, 1.0f);
  };
  fold(Axis::kMoveX, Action::kMoveRight, Action::kMoveLeft);
  fold(Axis::kMoveY, Action::kMoveBack, Action::kMoveForward);

  // Track the most recently active device for prompt glyphs.
  bool kbm_active = kbm.text_len > 0 || kbm.mouse_dx != 0 || kbm.mouse_dy != 0 || kbm.wheel != 0;
  for (int k = 0; !kbm_active && k < static_cast<int>(Key::kCount); ++k)
    if (kbm.pressed[k]) kbm_active = true;
  for (int m = 0; !kbm_active && m < static_cast<int>(MouseButton::kCount); ++m)
    if (kbm.mouse[m]) kbm_active = true;
  bool pad_active = false;
  if (pad.connected) {
    for (int b = 0; !pad_active && b < static_cast<int>(GamepadButton::kCount); ++b)
      if (pad.buttons[b]) pad_active = true;
    for (int x = 0; !pad_active && x < static_cast<int>(GamepadAxis::kCount); ++x) {
      bool trigger = x == static_cast<int>(GamepadAxis::kLeftTrigger) ||
                     x == static_cast<int>(GamepadAxis::kRightTrigger);
      f32 thr = trigger ? trigger_threshold : stick_deadzone;
      if (std::fabs(pad.axes[x]) > thr) pad_active = true;
    }
  }
  if (pad_active) last_device_ = InputDevice::kGamepad;
  else if (kbm_active) last_device_ = InputDevice::kKeyboardMouse;
  out->last_device = last_device_;
}

// --- Persistence ------------------------------------------------------------

namespace {

std::string Trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

}  // namespace

std::string InputMap::DefaultConfigPath() {
#if defined(_WIN32)
  if (const char* appdata = std::getenv("APPDATA"))
    return std::string(appdata) + "\\recreation\\controls.ini";
  return "";
#elif defined(__APPLE__)
  if (const char* home = std::getenv("HOME"))
    return std::string(home) + "/Library/Application Support/recreation/controls.ini";
  return "";
#else
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"))
    return std::string(xdg) + "/recreation/controls.ini";
  if (const char* home = std::getenv("HOME"))
    return std::string(home) + "/.config/recreation/controls.ini";
  return "";
#endif
}

bool InputMap::LoadFromIni(const std::string& path) {
  std::ifstream in(path);
  if (!in) return false;

  std::string section, line;
  while (std::getline(in, line)) {
    std::string t = Trim(line);
    if (t.empty() || t[0] == '#' || t[0] == ';') continue;
    if (t.front() == '[' && t.back() == ']') {
      section = t.substr(1, t.size() - 2);
      continue;
    }
    size_t eq = t.find('=');
    if (eq == std::string::npos) continue;
    std::string key = Trim(t.substr(0, eq));
    std::string value = Trim(t.substr(eq + 1));

    if (section == "options") {
      if (key == "look_sens_kbm") look_sens_kbm = std::strtof(value.c_str(), nullptr);
      else if (key == "look_sens_pad") look_sens_pad = std::strtof(value.c_str(), nullptr);
      else if (key == "invert_y") invert_y = value == "1" || value == "true";
      else if (key == "stick_deadzone") stick_deadzone = std::strtof(value.c_str(), nullptr);
      else if (key == "trigger_threshold") trigger_threshold = std::strtof(value.c_str(), nullptr);
      else if (key == "rumble") rumble = value == "1" || value == "true";
      else if (key == "adaptive_triggers") adaptive_triggers = value == "1" || value == "true";
      else if (key == "led" && value.size() == 6) {
        auto hex = [&](int i) { return static_cast<u8>(std::strtol(value.substr(i, 2).c_str(), nullptr, 16)); };
        led_r = hex(0);
        led_g = hex(2);
        led_b = hex(4);
      }
    } else if (section == "actions") {
      Action a;
      if (!ActionFromName(key.c_str(), &a)) continue;
      action_[idx(a)].clear();  // file overrides the default for this action
      std::stringstream ss(value);
      std::string tok;
      while (std::getline(ss, tok, ',')) {
        tok = Trim(tok);
        Binding b;
        if (!tok.empty() && BindingFromToken(tok.c_str(), &b)) AddBinding(a, b);
      }
    } else if (section == "axes") {
      Axis ax;
      if (!AxisFromName(key.c_str(), &ax)) continue;
      axis_[idx(ax)].clear();
      std::stringstream ss(value);
      std::string tok;
      while (std::getline(ss, tok, ',')) {
        tok = Trim(tok);
        Binding b;
        if (!tok.empty() && BindingFromToken(tok.c_str(), &b)) AddBinding(ax, b);
      }
    }
  }
  return true;
}

bool InputMap::SaveToIni(const std::string& path) const {
  std::ofstream out(path, std::ios::trunc);
  if (!out) return false;

  out << "# recreation controls. Tokens: key:W mouse:left pad:south padaxis:lefty+\n";
  out << "[options]\n";
  out << "look_sens_kbm=" << look_sens_kbm << "\n";
  out << "look_sens_pad=" << look_sens_pad << "\n";
  out << "invert_y=" << (invert_y ? 1 : 0) << "\n";
  out << "stick_deadzone=" << stick_deadzone << "\n";
  out << "trigger_threshold=" << trigger_threshold << "\n";
  out << "rumble=" << (rumble ? 1 : 0) << "\n";
  out << "adaptive_triggers=" << (adaptive_triggers ? 1 : 0) << "\n";
  char led[8];
  std::snprintf(led, sizeof(led), "%02x%02x%02x", led_r, led_g, led_b);
  out << "led=" << led << "\n\n";

  auto write_tokens = [&out](const std::vector<Binding>& v) {
    for (size_t i = 0; i < v.size(); ++i) {
      if (i) out << ",";
      out << BindingToken(v[i]);
    }
  };

  out << "[actions]\n";
  for (int a = 0; a < static_cast<int>(Action::kCount); ++a) {
    out << kActionNames[a] << "=";
    write_tokens(action_[a]);
    out << "\n";
  }
  out << "\n[axes]\n";
  for (int x = 0; x < static_cast<int>(Axis::kCount); ++x) {
    out << kAxisNames[x] << "=";
    write_tokens(axis_[x]);
    out << "\n";
  }
  return static_cast<bool>(out);
}

}  // namespace rec
