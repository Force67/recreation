#include "game_input.h"

#include "core/input.h"
#include "core/input_bindings.h"

// Recreation's input policy: the action/axis names that round-trip to
// controls.ini, the digital->analog movement folds, and the built-in
// keyboard/mouse + gamepad bindings. This is the game's control scheme, so it
// lives here rather than in the engine. The names and defaults match the layout
// the engine used to ship, so existing controls.ini files load unchanged.
namespace rx {

void RegisterGameInput(InputMap& map) {
  // INI names, in enum order. Must stay stable so old controls.ini files load.
  map.RegisterAction(Action::kMoveForward, "move_forward");
  map.RegisterAction(Action::kMoveBack, "move_back");
  map.RegisterAction(Action::kMoveLeft, "move_left");
  map.RegisterAction(Action::kMoveRight, "move_right");
  map.RegisterAction(Action::kJump, "jump");
  map.RegisterAction(Action::kSprint, "sprint");
  map.RegisterAction(Action::kSneak, "sneak");
  map.RegisterAction(Action::kCamUp, "cam_up");
  map.RegisterAction(Action::kCamDown, "cam_down");
  map.RegisterAction(Action::kActivate, "activate");
  map.RegisterAction(Action::kAttack, "attack");
  map.RegisterAction(Action::kReady, "ready");
  map.RegisterAction(Action::kThrowDebug, "throw_debug");
  map.RegisterAction(Action::kDropItem, "drop_item");
  map.RegisterAction(Action::kToggleWalk, "toggle_walk");
  map.RegisterAction(Action::kToggleThirdPerson, "toggle_third_person");
  map.RegisterAction(Action::kToggleJournal, "toggle_journal");
  map.RegisterAction(Action::kToggleEditor, "toggle_editor");
  map.RegisterAction(Action::kToggleMenu, "toggle_menu");
  map.RegisterAction(Action::kToggleDebug, "toggle_debug");
  map.RegisterAction(Action::kToggleTrace, "toggle_trace");
  map.RegisterAction(Action::kToggleQuests, "toggle_quests");
  map.RegisterAction(Action::kMenuUp, "menu_up");
  map.RegisterAction(Action::kMenuDown, "menu_down");
  map.RegisterAction(Action::kMenuLeft, "menu_left");
  map.RegisterAction(Action::kMenuRight, "menu_right");
  map.RegisterAction(Action::kMenuAccept, "menu_accept");
  map.RegisterAction(Action::kMenuCancel, "menu_cancel");
  map.RegisterAction(Action::kMenuTab, "menu_tab");
  map.RegisterAction(Action::kMenuPageLeft, "menu_page_left");
  map.RegisterAction(Action::kMenuPageRight, "menu_page_right");
  map.RegisterAction(Action::kEquipWeapon, "equip_weapon");

  map.RegisterAxis(Axis::kMoveX, "move_x");
  map.RegisterAxis(Axis::kMoveY, "move_y");
  map.RegisterAxis(Axis::kLookX, "look_x");
  map.RegisterAxis(Axis::kLookY, "look_y");

  // Fold keyboard movement into the move axes so gameplay reads one value.
  // Stick convention is SDL's (down/right positive), so forward subtracts on Y.
  map.RegisterFold(Axis::kMoveX, Action::kMoveRight, Action::kMoveLeft);
  map.RegisterFold(Axis::kMoveY, Action::kMoveBack, Action::kMoveForward);

  map.SetDefaultsFn([](InputMap& m) {
    auto key = [](Key k) { return Binding{SourceKind::kKey, static_cast<u16>(k), 0}; };
    auto mb = [](MouseButton b) { return Binding{SourceKind::kMouseButton, static_cast<u16>(b), 0}; };
    auto pad = [](GamepadButton g) {
      return Binding{SourceKind::kGamepadButton, static_cast<u16>(g), 0};
    };
    auto axis = [](GamepadAxis a, i8 dir) {
      return Binding{SourceKind::kGamepadAxis, static_cast<u16>(a), dir};
    };

    // Movement (keyboard discrete; the sticks feed the analog Axis values below).
    m.AddBinding(Action::kMoveForward, key(Key::kW));
    m.AddBinding(Action::kMoveBack, key(Key::kS));
    m.AddBinding(Action::kMoveLeft, key(Key::kA));
    m.AddBinding(Action::kMoveRight, key(Key::kD));
    m.AddBinding(Action::kJump, key(Key::kSpace));
    m.AddBinding(Action::kJump, pad(GamepadButton::kSouth));
    m.AddBinding(Action::kSprint, key(Key::kLeftShift));
    m.AddBinding(Action::kSprint, pad(GamepadButton::kLeftStick));
    m.AddBinding(Action::kSneak, key(Key::kLeftCtrl));
    m.AddBinding(Action::kSneak, pad(GamepadButton::kRightStick));
    m.AddBinding(Action::kCamUp, key(Key::kE));
    m.AddBinding(Action::kCamUp, pad(GamepadButton::kRightShoulder));
    m.AddBinding(Action::kCamDown, key(Key::kQ));
    m.AddBinding(Action::kCamDown, pad(GamepadButton::kLeftShoulder));

    // Gameplay verbs.
    m.AddBinding(Action::kActivate, key(Key::kE));
    m.AddBinding(Action::kActivate, pad(GamepadButton::kWest));
    m.AddBinding(Action::kAttack, mb(MouseButton::kLeft));
    m.AddBinding(Action::kAttack, axis(GamepadAxis::kRightTrigger, 1));
    m.AddBinding(Action::kReady, key(Key::kR));
    m.AddBinding(Action::kReady, pad(GamepadButton::kNorth));
    m.AddBinding(Action::kThrowDebug, key(Key::kF));
    // Drop the last picked-up item into the world (G is otherwise unbound).
    m.AddBinding(Action::kDropItem, key(Key::kG));

    // Mode toggles.
    m.AddBinding(Action::kToggleWalk, key(Key::kT));
    m.AddBinding(Action::kToggleThirdPerson, key(Key::kC));
    m.AddBinding(Action::kToggleJournal, key(Key::kJ));
    m.AddBinding(Action::kToggleJournal, pad(GamepadButton::kTouchpad));
    m.AddBinding(Action::kToggleEditor, key(Key::kF4));
    m.AddBinding(Action::kToggleMenu, key(Key::kEscape));
    m.AddBinding(Action::kToggleMenu, pad(GamepadButton::kStart));
    m.AddBinding(Action::kToggleDebug, key(Key::kF1));
    m.AddBinding(Action::kToggleTrace, key(Key::kF2));
    m.AddBinding(Action::kToggleQuests, key(Key::kF3));

    // Menu navigation: keyboard arrows + gamepad dpad and left stick.
    m.AddBinding(Action::kMenuUp, key(Key::kArrowUp));
    m.AddBinding(Action::kMenuUp, pad(GamepadButton::kDpadUp));
    m.AddBinding(Action::kMenuUp, axis(GamepadAxis::kLeftY, -1));
    m.AddBinding(Action::kMenuDown, key(Key::kArrowDown));
    m.AddBinding(Action::kMenuDown, pad(GamepadButton::kDpadDown));
    m.AddBinding(Action::kMenuDown, axis(GamepadAxis::kLeftY, 1));
    m.AddBinding(Action::kMenuLeft, key(Key::kArrowLeft));
    m.AddBinding(Action::kMenuLeft, pad(GamepadButton::kDpadLeft));
    m.AddBinding(Action::kMenuLeft, axis(GamepadAxis::kLeftX, -1));
    m.AddBinding(Action::kMenuRight, key(Key::kArrowRight));
    m.AddBinding(Action::kMenuRight, pad(GamepadButton::kDpadRight));
    m.AddBinding(Action::kMenuRight, axis(GamepadAxis::kLeftX, 1));
    m.AddBinding(Action::kMenuAccept, key(Key::kReturn));
    m.AddBinding(Action::kMenuAccept, pad(GamepadButton::kSouth));
    m.AddBinding(Action::kMenuCancel, key(Key::kEscape));
    m.AddBinding(Action::kMenuCancel, pad(GamepadButton::kEast));
    m.AddBinding(Action::kMenuTab, key(Key::kTab));
    m.AddBinding(Action::kMenuPageLeft, pad(GamepadButton::kLeftShoulder));
    m.AddBinding(Action::kMenuPageRight, pad(GamepadButton::kRightShoulder));

    // Draw / sheathe a weapon in first person (X is otherwise unbound; pad Y is
    // shared with Ready Weapon, the natural equivalent).
    m.AddBinding(Action::kEquipWeapon, key(Key::kX));
    m.AddBinding(Action::kEquipWeapon, pad(GamepadButton::kNorth));

    // Analog axes: left stick drives movement, right stick drives the look.
    m.AddAxisBinding(Axis::kMoveX, axis(GamepadAxis::kLeftX, 0));
    m.AddAxisBinding(Axis::kMoveY, axis(GamepadAxis::kLeftY, 0));
    m.AddAxisBinding(Axis::kLookX, axis(GamepadAxis::kRightX, 0));
    m.AddAxisBinding(Axis::kLookY, axis(GamepadAxis::kRightY, 0));
  });
}

}  // namespace rx
