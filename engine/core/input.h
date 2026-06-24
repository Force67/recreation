#ifndef RECREATION_CORE_INPUT_H_
#define RECREATION_CORE_INPUT_H_

#include "core/types.h"

namespace rec {

// Just the keys the engine binds. Backends translate their native codes,
// unknown keys drop.
enum class Key : u8 {
  kW,
  kA,
  kS,
  kD,
  kQ,
  kE,
  kF,
  kT,
  kC,
  kR,  // editor: rotate the selection
  kG,  // editor: grab / move the selection
  kX,  // editor: delete the selection
  kZ,  // editor: undo (with Ctrl)
  kB,  // editor: toggle the asset browser
  kV,  // editor: duplicate the selection (with Ctrl)
  kSpace,
  kLeftShift,
  kLeftCtrl,
  kEscape,
  kF1,
  kF2,
  kF3,
  kF4,  // toggle the map editor
  kF5,  // editor: save the layout
  kDelete,     // editor: delete the selection
  kBackspace,  // text fields: erase
  kReturn,     // text fields: commit
  k1,  // dialogue option select; also journal pick when the journal is open
  k2,
  k3,
  k4,
  kJ,  // toggle the quest journal
  // Menu navigation keys. Appended after the original set so the numeric codes
  // of the keys above stay stable for the C# KeyPressed bridge.
  kArrowUp,
  kArrowDown,
  kArrowLeft,
  kArrowRight,
  kTab,
  kCount,
};

enum class MouseButton : u8 { kLeft, kRight, kMiddle, kCount };

// Gamepad buttons, named by position (SDL's modern convention) so PS5 and Xbox
// pads map the same way: kSouth is Cross/A, kEast is Circle/B, etc. Backends
// translate their native codes; unknown buttons drop.
enum class GamepadButton : u8 {
  kSouth,   // A / Cross
  kEast,    // B / Circle
  kWest,    // X / Square
  kNorth,   // Y / Triangle
  kBack,    // View / Share
  kGuide,   // Xbox / PS button
  kStart,   // Menu / Options
  kLeftStick,
  kRightStick,
  kLeftShoulder,
  kRightShoulder,
  kDpadUp,
  kDpadDown,
  kDpadLeft,
  kDpadRight,
  kTouchpad,  // DualSense touchpad click (absent on Xbox)
  kCount,
};

enum class GamepadAxis : u8 {
  kLeftX,
  kLeftY,
  kRightX,
  kRightY,
  kLeftTrigger,
  kRightTrigger,
  kCount,
};

// Polled gamepad state, filled alongside InputState during PumpEvents. Sticks
// are [-1,1] (down/right positive); triggers are [0,1]. Kept separate from
// InputState so the keyboard/mouse path and the C# key bridge stay untouched.
struct GamepadState {
  enum class Kind : u8 { kUnknown, kXbox, kDualSense };
  bool connected = false;
  Kind kind = Kind::kUnknown;
  bool buttons[static_cast<u8>(GamepadButton::kCount)] = {};
  bool pressed[static_cast<u8>(GamepadButton::kCount)] = {};  // went down this pump
  f32 axes[static_cast<u8>(GamepadAxis::kCount)] = {};

  bool button(GamepadButton b) const { return buttons[static_cast<u8>(b)]; }
  bool button_pressed(GamepadButton b) const { return pressed[static_cast<u8>(b)]; }
  f32 axis(GamepadAxis a) const { return axes[static_cast<u8>(a)]; }
};

// DualSense adaptive-trigger request. A no-op on pads that lack the feature
// (Xbox), so callers can issue it unconditionally.
struct TriggerEffect {
  enum class Type : u8 { kOff, kResistance, kWeapon, kVibration };
  Type type = Type::kOff;
  u8 start = 0;     // 0-255: position along the pull where the effect begins
  u8 strength = 0;  // 0-255: resistance / vibration amplitude
};

// Polled state the window backend fills during PumpEvents. Deltas cover one
// pump and reset on the next.
struct InputState {
  bool keys[static_cast<u8>(Key::kCount)] = {};
  bool pressed[static_cast<u8>(Key::kCount)] = {};  // went down this pump
  bool mouse[static_cast<u8>(MouseButton::kCount)] = {};
  f32 mouse_dx = 0;
  f32 mouse_dy = 0;
  // Absolute cursor position in window pixels, persisted across pumps. Used by
  // the gui (the camera uses the deltas above).
  f32 mouse_x = 0;
  f32 mouse_y = 0;
  f32 wheel = 0;
  // UTF-8 text typed this pump (resets each PumpEvents, like the deltas). Filled
  // from the platform's text-input events so editor text fields (the asset
  // search box) can read characters without the engine binding every key.
  char text[32] = {};
  u8 text_len = 0;

  bool key(Key k) const { return keys[static_cast<u8>(k)]; }
  bool key_pressed(Key k) const { return pressed[static_cast<u8>(k)]; }
  bool button(MouseButton b) const { return mouse[static_cast<u8>(b)]; }
};

}  // namespace rec

#endif  // RECREATION_CORE_INPUT_H_
