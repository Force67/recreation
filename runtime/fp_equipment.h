#ifndef RECREATION_RUNTIME_FP_EQUIPMENT_H_
#define RECREATION_RUNTIME_FP_EQUIPMENT_H_

#include "core/types.h"
#include "inventory/item_catalog.h"

namespace rx {

struct EngineContext;
class ActorSystem;

// First-person weapon policy: turns the player's rx::inventory into a drawn
// first-person weapon. It owns the equip/idle/swing/sheathe *state machine*,
// polls the equip key, resolves the "most recent weapon" from the item bridge's
// inventory + catalog, and calls ActorSystem's first-person rig primitives
// (which own the actual _1stperson skeleton, arm meshes, clip playback and
// rendering). Kept separate from ActorSystem because this is game policy, while
// ActorSystem is the skinning/clip/render machinery.
//
// Visibility rule: the first-person layer renders only while a weapon is drawn
// AND the view is first person AND the player is walk-mode + spawned. Switching
// to third person pops the layer at once (the logical draw state is kept, so
// switching back to first person resumes the held idle without re-drawing).
class FpEquipment {
 public:
  FpEquipment(EngineContext& ctx, ActorSystem& actors);

  // Per-frame: run the state machine, poll input, re-root the rig to the camera
  // and refresh the render/advance flags. Called from ActorSystem::Update after
  // the camera has published this frame's walk eye/target.
  void Update(f32 dt);

 private:
  enum class State {
    kSheathed,     // no weapon in hand; the FP layer is inactive
    kEquipping,    // playing the draw clip; weapon attached, settling into idle
    kDrawn,        // weapon held, looping the idle
    kAttacking,    // playing a one-shot swing over the idle
    kUnequipping,  // playing the sheathe clip; detaches + hides on completion
  };

  // Equips the most recently picked-up weapon stack from the player inventory
  // (scans the inventory back-to-front for a WEAP def via the item catalog),
  // marks it equipped in rx::Equipment under the hand.right tag, attaches its
  // hand mesh to the FP rig and plays the draw clip. No-op (with a HUD toast)
  // when the inventory holds no weapon or the rig assets are missing.
  void BeginEquip();
  // Plays the sheathe clip; the weapon detaches and the slot clears when it ends.
  void BeginUnequip();

  // Menu / text-field capture: gameplay keys are ignored while a modal owns input.
  bool InputBlocked() const;

  // RX_FP_PROBE=<dir> debug harness: a few seconds after the world settles, force
  // first person, pick up + draw the nearest weapon (falling back to any WEAP
  // base's hand mesh when the interior has no loose weapon), settle into the
  // idle, swing, and capture rec_fp_sword_idle.png / rec_fp_sword_swing.png into
  // <dir>. Exercises the whole FP path on a headless GPU run without input.
  void MaybeRunProbe(f32 dt);
  bool ProbePickupNearestWeapon();  // best-effort inventory pickup of a loose WEAP
  void ProbeForceDraw();            // drive the rig directly off any WEAP base mesh

  EngineContext& ctx_;
  ActorSystem& actors_;
  State state_ = State::kSheathed;
  inventory::ItemDefId equipped_item_ = inventory::kInvalidItemDef;

  int probe_phase_ = 0;
  f32 probe_t_ = 0;
  bool probe_forced_view_ = false;
  bool swing_shot_ = false;
};

}  // namespace rx

#endif  // RECREATION_RUNTIME_FP_EQUIPMENT_H_
