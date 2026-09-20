#ifndef RECREATION_RUNTIME_ACTOR_PLAYER_TUNING_H_
#define RECREATION_RUNTIME_ACTOR_PLAYER_TUNING_H_

#include <base/containers/unordered_map.h>

#include "character/character.h"
#include "components/bethesda/movement_type.h"

namespace rx::player_tuning {

// The one source of the player character's tuning: movement speeds decoded from
// the game's MOVT records (documented Skyrim defaults where a record is
// missing) and the capsule/eye geometry. The local player controller and the
// server-side remote-player driver both assemble from here, so a remote body
// accelerates, jumps and crouches with exactly the feel of the local one.
inline constexpr f32 kUnit = 0.0142857f;  // Skyrim world unit -> metres

inline character::CharacterMovementSettings BuildMovementSettings(
    bethesda::RecordStore& records) {
  constexpr f32 kDefaultWalkUnits = 80.10f;    // MOVT NPC_Default_MT forward walk
  constexpr f32 kDefaultRunUnits = 370.0f;     // MOVT NPC_Default_MT forward run
  constexpr f32 kDefaultSprintUnits = 500.0f;  // MOVT NPC_Sprinting_MT forward
  constexpr f32 kDefaultSneakUnits = 222.0f;   // MOVT NPC_Sneaking_MT forward run

  base::UnorderedMap<u64, bethesda::MovementType> movement_types;
  const int n = bethesda::LoadMovementTypes(records, &movement_types);
  auto speed_units = [&](const char* editor_id, bool run, f32 fallback) -> f32 {
    if (const bethesda::MovementType* mt =
            bethesda::FindMovementType(movement_types, editor_id);
        mt && mt->has_speeds) {
      const f32 v = run ? mt->forward_run : mt->forward_walk;
      if (v > 1.0f)
        return v;
    }
    return fallback;
  };

  character::CharacterMovementSettings move;
  move.walk_speed = speed_units("NPC_Default_MT", false, kDefaultWalkUnits) * kUnit;       // MOVT
  move.run_speed = speed_units("NPC_Default_MT", true, kDefaultRunUnits) * kUnit;          // MOVT
  move.sprint_speed = speed_units("NPC_Sprinting_MT", true, kDefaultSprintUnits) * kUnit;  // MOVT
  move.crouch_speed = speed_units("NPC_Sneaking_MT", true, kDefaultSneakUnits) * kUnit;    // MOVT
  move.ground_acceleration = 45.0f;  // TUNED: Skyrim reaches full speed in ~0.12 s; keeps
                                     // starts snappy under the engine's 0.18 s gait blend.
  move.ground_deceleration = 55.0f;  // TUNED: > accel so stops read crisp (stop epsilon zeroes)
  move.air_control = 0.2f;           // TUNED: Skyrim has near-zero air control
  move.jump_height = 76.0f * kUnit;  // GMST fJumpHeightMin = 76 u -> 1.086 m apex (arc timing
                                     // set by gravity below; apex height is gravity-independent)
  // GRAVITY DECISION: the engine default is 16.0 m/s^2 (~1.6 g, brisk). Real Skyrim's
  // jump reads slightly floaty (its fall is Havok-driven, ~9.8), but the priority here is
  // "responsive and nice to control". At 1.086 m apex, 16.0 gives a ~0.37 s rise / ~0.74 s
  // total arc versus ~0.47 s / ~0.94 s at 9.81 -- noticeably snappier without changing the
  // jump height or the Skyrim silhouette. In-game the brisk arc reads better; the floaty
  // 9.81 arc felt mushy on landings. Set EXPLICITLY (not inherited) so the choice is visible.
  move.gravity = 16.0f;               // TUNED: brisk, responsive jump/fall arc (was 9.81)
  move.step_height = 0.4f;            // TUNED
  move.max_slope_angle = 0.9599311f;  // ~55 deg, TUNED

  // --- Game-feel: body-yaw turn smoothing (engine-driven, third person only) ---
  // StepCharacters eases CharacterState.facing_yaw toward the movement direction when the
  // entity carries CharacterViewMode{ThirdPerson}; first person hard-locks it to the raw
  // look yaw. Started from the engine defaults; observed in the Bannered Mare, they read
  // responsive yet weighty for a humanoid, so they stay.
  move.turn_half_life = 0.09f;        // TUNED (engine default): eased facing chase
  move.pivot_turn_half_life = 0.05f;  // TUNED (engine default): faster chase for ~180 reversals
  move.pivot_angle = 2.4434610f;      // ~140 deg: beyond this the pivot rate applies
  // --- Game-feel: gait target-speed blend + crisp stop --------------------------
  move.speed_blend_time = 0.18f;    // TUNED (engine default): walk<->run<->sprint target blend
  move.stop_speed_epsilon = 0.05f;  // TUNED (engine default): zero horizontal vel below this
  // --- Game-feel: jump forgiveness (invisible responsiveness, no authenticity cost) ---
  move.jump_buffer_time = 0.12f;  // TUNED (engine default): pre-land buffered jump window
  move.coyote_time = 0.12f;       // TUNED (engine default): post-ledge grace window

  (void)n;  // the count only feeds the controller's boot log
  return move;
}

inline character::CharacterShape BuildCharacterShape() {
  character::CharacterShape shape;
  // The actor capsule is radius 0.3, cylinder half-height 0.55 (total 1.7 m); the
  // shape's standing dims match so StepCharacters never resizes on the first step.
  shape.standing_radius = 0.3f;
  shape.standing_height = 1.7f;
  shape.crouched_radius = 0.3f;
  shape.crouched_height = 1.25f;       // TUNED: Skyrim sneak crouch
  shape.standing_eye_height = 1.715f;  // GMST-adjacent: ~120 u FP eye (matches prior 1.7 m)
  shape.crouched_eye_height = 1.05f;   // TUNED: sneak eye drop (animation-driven in Skyrim)
  shape.crouch_blend_speed = 9.0f;     // TUNED: smooth sneak enter/exit blend
  // --- Game-feel: eye vertical smoothing + landing dip (Skyrim itself dips) ------
  // The camera anchor's vertical eases over stairs/steps so the eye glides; horizontal
  // stays raw. A subtle, fast-recovering dip on real landings. Engine defaults read right
  // on the Bannered Mare stairs (eye glides, no head-pop) so they stay explicit.
  shape.eye_step_half_life = 0.06f;     // TUNED (engine default): grounded vertical eye smoothing
  shape.landing_dip_min_speed = 2.5f;   // TUNED (engine default): no dip below this impact speed
  shape.landing_dip_scale = 0.03f;      // TUNED: metres of dip per m/s over min
  shape.landing_dip_max = 0.14f;        // TUNED (engine default): subtle hard cap
  shape.landing_dip_half_life = 0.09f;  // TUNED (engine default): fast recovery
  return shape;
}

}  // namespace rx::player_tuning

#endif  // RECREATION_RUNTIME_ACTOR_PLAYER_TUNING_H_
