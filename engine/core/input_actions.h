#ifndef RECREATION_CORE_INPUT_ACTIONS_H_
#define RECREATION_CORE_INPUT_ACTIONS_H_

#include "core/types.h"

namespace rec {

// Semantic, device-agnostic input. Gameplay queries these instead of raw keys
// so the same code serves keyboard/mouse and gamepad, and bindings can be
// remapped at runtime (see InputMap in input_bindings.h). Raw InputState /
// GamepadState stay available for text fields, the editor, and the C# bridge.
enum class Action : u8 {
  // Movement (digital; analog sticks feed the Axis values below as well).
  kMoveForward,
  kMoveBack,
  kMoveLeft,
  kMoveRight,
  kJump,
  kSprint,
  kSneak,
  kCamUp,    // free-fly camera rise (Q)
  kCamDown,  // free-fly camera descend (E)
  // Gameplay verbs.
  kActivate,
  kAttack,
  kReady,
  kThrowDebug,
  // Mode toggles.
  kToggleWalk,
  kToggleThirdPerson,
  kToggleJournal,
  kToggleEditor,
  kToggleMenu,
  // Debug overlays.
  kToggleDebug,
  kToggleTrace,
  kToggleQuests,
  // Menu navigation (also drives the ugui focus ring).
  kMenuUp,
  kMenuDown,
  kMenuLeft,
  kMenuRight,
  kMenuAccept,
  kMenuCancel,
  kMenuTab,
  kMenuPageLeft,
  kMenuPageRight,
  kCount,
};

// Analog axes in [-1,1] (down/right positive). Look axes carry the right stick;
// mouse look stays on InputState deltas. Move axes combine stick deflection with
// the digital movement actions so gameplay can read one continuous value.
enum class Axis : u8 { kMoveX, kMoveY, kLookX, kLookY, kCount };

enum class InputDevice : u8 { kKeyboardMouse, kGamepad };

// Resolved per pump from InputState + GamepadState through an InputMap.
struct ActionState {
  bool held[static_cast<u8>(Action::kCount)] = {};
  bool edge[static_cast<u8>(Action::kCount)] = {};  // went down this pump
  f32 analog[static_cast<u8>(Axis::kCount)] = {};
  // Which device produced the most recent input, for prompt glyphs.
  InputDevice last_device = InputDevice::kKeyboardMouse;

  bool down(Action a) const { return held[static_cast<u8>(a)]; }
  bool pressed(Action a) const { return edge[static_cast<u8>(a)]; }
  f32 axis(Axis a) const { return analog[static_cast<u8>(a)]; }
};

}  // namespace rec

#endif  // RECREATION_CORE_INPUT_ACTIONS_H_
