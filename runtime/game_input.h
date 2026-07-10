#ifndef RECREATION_RUNTIME_GAME_INPUT_H_
#define RECREATION_RUNTIME_GAME_INPUT_H_

#include "core/input_actions.h"

namespace rx {

class InputMap;

// Recreation's action set. The engine (rx) owns no gameplay verbs, so the game
// defines which actions and axes exist and registers their INI names + default
// bindings with an InputMap at startup (see RegisterGameInput).
//
// Order is preserved from when this lived in the engine, but the numeric values
// are an internal detail: they do not cross the C# bridge (only the Key enum
// does), and controls.ini serialises action *names*, so existing user configs
// keep loading regardless of the ordering here.
enum class Action : ActionId {
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
enum class Axis : AxisId { kMoveX, kMoveY, kLookX, kLookY, kCount };

// Registers the game's action/axis names, digital->analog folds and the default
// keyboard/mouse + gamepad bindings with `map`. Call once at startup, before the
// controls.ini is loaded.
void RegisterGameInput(InputMap& map);

}  // namespace rx

#endif  // RECREATION_RUNTIME_GAME_INPUT_H_
