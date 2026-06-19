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
  kSpace,
  kLeftShift,
  kLeftCtrl,
  kEscape,
  kF1,
  kCount,
};

enum class MouseButton : u8 { kLeft, kRight, kMiddle, kCount };

// Polled state the window backend fills during PumpEvents. Deltas cover one
// pump and reset on the next.
struct InputState {
  bool keys[static_cast<u8>(Key::kCount)] = {};
  bool pressed[static_cast<u8>(Key::kCount)] = {};  // went down this pump
  bool mouse[static_cast<u8>(MouseButton::kCount)] = {};
  f32 mouse_dx = 0;
  f32 mouse_dy = 0;
  f32 wheel = 0;

  bool key(Key k) const { return keys[static_cast<u8>(k)]; }
  bool key_pressed(Key k) const { return pressed[static_cast<u8>(k)]; }
  bool button(MouseButton b) const { return mouse[static_cast<u8>(b)]; }
};

}  // namespace rec

#endif  // RECREATION_CORE_INPUT_H_
