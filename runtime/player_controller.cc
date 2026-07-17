#include "player_controller.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "actor_system.h"
#include "character/character.h"
#include "core/input.h"
#include "core/input_actions.h"
#include "core/input_bindings.h"
#include "core/log.h"
#include "core/math.h"
#include "ecs/world.h"
#include "engine_context.h"
#include "game_input.h"
#include "scene/camera.h"
#include "scene/camera_rig.h"
#include "scene/components.h"

// Skyrim-authentic player locomotion + camera. See player_controller.h.
//
// Tuning table (each value tagged with its source):
//   MOVT  = decoded at runtime from the loaded Skyrim.esm MOVT/SPED records
//           (NPC_Default_MT / NPC_Sprinting_MT / NPC_Sneaking_MT), forward
//           component, units/s * 0.0142857 -> m/s. Falls back to the documented
//           SE default below when a record is missing.
//   GMST  = documented Special Edition game-setting default (verified against
//           Skyrim.esm: fJumpHeightMin = 76 units).
//   INI   = documented [Camera] Skyrim_default.ini default (units * 0.0142857).
//   TUNED = no governing Skyrim setting (animation-driven in the original);
//           chosen to read right on recreation's 1.7 m player capsule.
namespace rx {
namespace {

// 1 Bethesda game unit in engine metres (1/70). Matches the engine-wide scale
// (game_profile.units_to_meters); used to convert MOVT/GMST/INI unit values.
constexpr f32 kUnit = 0.0142857f;

// Documented SE fallbacks for the MOVT-decoded speeds (units/s), used when the
// movement type record is absent from the loaded data.
constexpr f32 kDefaultWalkUnits = 80.10f;    // MOVT NPC_Default_MT forward walk
constexpr f32 kDefaultRunUnits = 370.0f;     // MOVT NPC_Default_MT forward run
constexpr f32 kDefaultSprintUnits = 500.0f;  // MOVT NPC_Sprinting_MT forward
constexpr f32 kDefaultSneakUnits = 222.0f;   // MOVT NPC_Sneaking_MT forward run

f32 WrapPi(f32 a) {
  while (a > 3.14159265f) a -= 6.2831853f;
  while (a < -3.14159265f) a += 6.2831853f;
  return a;
}

// Camera-space forward heading (rx convention): forward(yaw) = (sin, 0, -cos).
Vec3 ForwardFromYaw(f32 yaw) { return {std::sin(yaw), 0.0f, -std::cos(yaw)}; }

}  // namespace

PlayerController::PlayerController(EngineContext& ctx, ActorSystem& actors,
                                  const InputMap& input_map)
    : ctx_(ctx), actors_(actors), input_map_(input_map) {}

PlayerController::~PlayerController() = default;

bool PlayerController::Assemble() {
  if (assembled_) return true;
  if (!actors_.HasPlayer()) return false;
  ecs::World& world = *ctx_.world;

  player_ = actors_.PlayerEntity();
  const physics::CharacterId cid = actors_.PlayerCharacter();
  if (player_ == ecs::kInvalidEntity || cid == 0) return false;

  // --- Movement settings: MOVT-decoded speeds, documented defaults elsewhere --
  const int n = bethesda::LoadMovementTypes(*ctx_.records, &movement_types_);
  auto speed_units = [&](const char* editor_id, bool run, f32 fallback) -> f32 {
    if (const bethesda::MovementType* mt =
            bethesda::FindMovementType(movement_types_, editor_id);
        mt && mt->has_speeds) {
      const f32 v = run ? mt->forward_run : mt->forward_walk;
      if (v > 1.0f) return v;
    }
    return fallback;
  };
  character::CharacterMovementSettings move;
  move.walk_speed = speed_units("NPC_Default_MT", false, kDefaultWalkUnits) * kUnit;  // MOVT
  move.run_speed = speed_units("NPC_Default_MT", true, kDefaultRunUnits) * kUnit;      // MOVT
  move.sprint_speed = speed_units("NPC_Sprinting_MT", true, kDefaultSprintUnits) * kUnit;  // MOVT
  move.crouch_speed = speed_units("NPC_Sneaking_MT", true, kDefaultSneakUnits) * kUnit;    // MOVT
  move.ground_acceleration = 45.0f;  // TUNED: Skyrim reaches full speed in ~0.12 s
  move.ground_deceleration = 55.0f;  // TUNED
  move.air_control = 0.2f;           // TUNED: Skyrim has near-zero air control
  move.jump_height = 76.0f * kUnit;  // GMST fJumpHeightMin = 76 u -> 1.086 m apex
  move.gravity = 9.81f;              // TUNED (Skyrim jump falling is Havok-driven)
  move.step_height = 0.4f;           // TUNED
  move.max_slope_angle = 0.9599311f;  // ~55 deg, TUNED

  RX_INFO(
      "player: MOVT types={} -> walk {:.3f} run {:.3f} sprint {:.3f} sneak {:.3f} m/s, jump {:.3f} m",
      n, move.walk_speed, move.run_speed, move.sprint_speed, move.crouch_speed, move.jump_height);

  // --- Capsule + eye geometry -------------------------------------------------
  // The actor capsule is radius 0.3, cylinder half-height 0.55 (total 1.7 m); the
  // shape's standing dims match so StepCharacters never resizes on the first step.
  character::CharacterShape shape;
  shape.standing_radius = 0.3f;
  shape.standing_height = 1.7f;
  shape.crouched_radius = 0.3f;
  shape.crouched_height = 1.25f;       // TUNED: Skyrim sneak crouch
  shape.standing_eye_height = 1.715f;  // GMST-adjacent: ~120 u FP eye (matches prior 1.7 m)
  shape.crouched_eye_height = 1.05f;   // TUNED: sneak eye drop (animation-driven in Skyrim)
  shape.crouch_blend_speed = 9.0f;

  // --- Third-person camera rig (INI-sourced offsets/zoom) ---------------------
  view_settings_.fp_pitch_limit = 1.4835f;      // ~85 deg
  view_settings_.tp_distance = 2.6f;            // TUNED default rest distance
  view_settings_.tp_min_distance = 155.0f * kUnit;  // INI fVanityModeMinDist 155 u
  view_settings_.tp_max_distance = 600.0f * kUnit;  // INI fVanityModeMaxDist 600 u
  view_settings_.tp_shoulder_offset = 30.0f * kUnit;  // INI fOverShoulderPosX 30 u
  view_settings_.tp_height_offset = -10.0f * kUnit;   // INI fOverShoulderPosZ -10 u
  view_settings_.tp_pitch_min = -1.1f;
  view_settings_.tp_pitch_max = 1.2f;
  view_settings_.tp_position_half_life = 0.08f;
  view_settings_.tp_rotation_half_life = 0.05f;
  view_settings_.tp_obstruction_radius = 0.25f;
  view_settings_.tp_obstruction_margin = 0.12f;

  // Current feet position from the actor world transform (entity origin = feet).
  Vec3 feet{};
  actors_.PlayerWorldPos(&feet);
  const f32 radius = shape.standing_radius;
  const f32 half_height = std::max(shape.standing_height * 0.5f - radius, 0.01f);

  world.Add(player_, scene::Transform{.position = {feet.x, feet.y, feet.z}});
  world.Add(player_, move);
  world.Add(player_, shape);
  world.Add(player_, character::CharacterIntent{});
  world.Add(player_, character::CharacterState{});
  world.Add(player_, character::CharacterViewMode{});
  world.Add(player_, character::CharacterBody{cid, radius, half_height, false});

  // Seed heading from the actor facing so the FP look and the body agree at spawn.
  f32 spawn_yaw = actors_.PlayerYaw();
  // Debug/capture hook: RX_PLAYER_YAW=<degrees> overrides the spawn heading so a
  // scripted capture can face the player into the room instead of the exit door.
  if (const char* y = std::getenv("RX_PLAYER_YAW"))
    spawn_yaw = std::atof(y) * 3.14159265358979323846f / 180.0f;
  if (auto* st = world.Get<character::CharacterState>(player_)) st->yaw = spawn_yaw;
  cam_yaw_ = spawn_yaw;
  cam_pitch_ = 0.0f;
  facing_yaw_ = spawn_yaw;

  // Debug/capture hook: RX_PLAYER_VIEW=fp|tp forces the initial view mode.
  if (const char* v = std::getenv("RX_PLAYER_VIEW")) {
    if (std::strcmp(v, "fp") == 0) ctx_.third_person = false;
    else if (std::strcmp(v, "tp") == 0) ctx_.third_person = true;
  }

  // ctx_.third_person is the live source of truth (the FP-equipment layer gates on
  // it). Present the matching rx view kind before building the rig.
  if (auto* vm = world.Get<character::CharacterViewMode>(player_)) {
    vm->kind = ctx_.third_person ? character::CharacterViewKind::kThirdPerson
                                 : character::CharacterViewKind::kFirstPerson;
  }
  was_third_person_ = ctx_.third_person;
  character::ApplyCharacterViewMode(world, player_, view_settings_);

  camera_output_ = world.Create();
  scene::InitializeCameraStack(world, camera_output_, player_);

  assembled_ = true;
  RX_INFO("player controller: rx character + camera rig assembled ({} person)",
          ctx_.third_person ? "third" : "first");
  return true;
}

void PlayerController::ReconcileViewMode() {
  ecs::World& world = *ctx_.world;
  auto* vm = world.Get<character::CharacterViewMode>(player_);
  if (!vm) return;
  const bool is_tp = vm->kind == character::CharacterViewKind::kThirdPerson;
  if (is_tp == ctx_.third_person) return;  // already in sync

  // Seed the free-orbit / look heading so the FP<->TP cut does not jump.
  auto* st = world.Get<character::CharacterState>(player_);
  if (ctx_.third_person) {
    // FP -> TP: carry the FP look yaw into the free-orbit yaw.
    if (st) cam_yaw_ = st->yaw;
  } else {
    // TP -> FP: face the body/look where the third-person camera pointed.
    if (st) st->yaw = cam_yaw_;
  }
  character::ToggleCharacterViewMode(world, player_, camera_output_, player_, view_settings_,
                                     {.duration = 0.25f});
  was_third_person_ = ctx_.third_person;
}

void PlayerController::ApplyZoom(f32 wheel) {
  ecs::World& world = *ctx_.world;
  auto* vm = world.Get<character::CharacterViewMode>(player_);
  if (!vm || wheel == 0.0f) return;
  const f32 range = view_settings_.tp_max_distance - view_settings_.tp_min_distance;
  const f32 step = wheel * 0.075f * range;  // INI fMouseWheelZoomIncrement = 0.075 of range/tick

  if (vm->kind == character::CharacterViewKind::kThirdPerson) {
    auto* boom = world.Get<scene::CameraBoom>(player_);
    if (!boom) return;
    const f32 desired = boom->distance - step;  // scroll up (wheel>0) zooms in
    if (desired < view_settings_.tp_min_distance - 1e-3f && wheel > 0.0f) {
      ctx_.third_person = false;  // zoom past the minimum -> first person (smooth toggle next reconcile)
      return;
    }
    boom->distance =
        std::clamp(desired, view_settings_.tp_min_distance, view_settings_.tp_max_distance);
  } else if (wheel < 0.0f) {
    // First person, scroll down -> back to third person at the minimum distance.
    ctx_.third_person = true;
    if (auto* boom = world.Get<scene::CameraBoom>(player_))
      boom->distance = view_settings_.tp_min_distance;
  }
}

void PlayerController::FillIntent(const InputState& input, const ActionState& actions, bool allow,
                                  bool auto_walk_active, const Vec3& auto_move, f32 dt) {
  ecs::World& world = *ctx_.world;
  auto* intent = world.Get<character::CharacterIntent>(player_);
  auto* state = world.Get<character::CharacterState>(player_);
  auto* vm = world.Get<character::CharacterViewMode>(player_);
  if (!intent || !state || !vm) return;
  const bool tp = vm->kind == character::CharacterViewKind::kThirdPerson;

  // --- Look ------------------------------------------------------------------
  f32 yaw_delta = 0, pitch_delta = 0;
  if (allow) {
    const f32 inv = input_map_.invert_y ? -1.0f : 1.0f;
    yaw_delta = input.mouse_dx * input_map_.look_sens_kbm;
    pitch_delta = -input.mouse_dy * input_map_.look_sens_kbm * inv;
    yaw_delta += actions.axis(Axis::kLookX) * input_map_.look_sens_pad * dt;
    pitch_delta -= actions.axis(Axis::kLookY) * input_map_.look_sens_pad * dt * inv;
  }
  cam_pitch_ = std::clamp(cam_pitch_ + pitch_delta,
                          tp ? view_settings_.tp_pitch_min : -view_settings_.fp_pitch_limit,
                          tp ? view_settings_.tp_pitch_max : view_settings_.fp_pitch_limit);
  // Debug/capture hook: a scripted probe can pin the first-person look pitch to
  // aim at the floor (e.g. to frame a dropped item) via ctx_.debug_look_pitch.
  if (!tp && ctx_.debug_look_pitch < 1e8f)
    cam_pitch_ = std::clamp(ctx_.debug_look_pitch, -view_settings_.fp_pitch_limit,
                            view_settings_.fp_pitch_limit);

  // --- Move input ------------------------------------------------------------
  f32 fwd = 0, right = 0;
  bool sprint = false, crouch = false, jump = false;
  if (auto_walk_active) {
    // Quest-guided auto-walk: run along the supplied world direction, ease the
    // camera to trail it so the path stays framed.
    Vec3 m = auto_move;
    const f32 l = Length(m);
    if (l > 1e-3f) {
      const f32 target = std::atan2(m.x, -m.z);  // camera yaw whose forward == move
      if (tp) cam_yaw_ = WrapPi(cam_yaw_ + WrapPi(target - cam_yaw_) * std::min(1.0f, dt * 4.0f));
      else state->yaw = WrapPi(state->yaw + WrapPi(target - state->yaw) * std::min(1.0f, dt * 4.0f));
    }
    intent->move = l > 1e-3f ? m * (1.0f / l) : Vec3{0, 0, 0};
    intent->gait = character::CharacterGait::kRun;
    intent->crouch = false;
    intent->look_yaw_delta = 0;
    intent->look_pitch_delta = 0;
    return;
  }
  if (allow) {
    fwd = -actions.axis(Axis::kMoveY);  // W = forward (SDL stick-down is +)
    right = actions.axis(Axis::kMoveX);
    sprint = actions.down(Action::kSprint);
    crouch = actions.down(Action::kSneak);
    jump = actions.pressed(Action::kJump);
  }

  // Camera-relative world move. FP uses the body/look heading (state.yaw); TP
  // uses the free-orbit camera yaw so W walks where the camera faces.
  const f32 move_yaw = tp ? cam_yaw_ : state->yaw;
  const Vec3 f = ForwardFromYaw(move_yaw);
  const Vec3 r{-f.z, 0.0f, f.x};  // right = forward rotated -90 about +Y
  Vec3 move = f * fwd + r * right;
  const f32 len = Length(move);
  if (len > 1.0f) move = move * (1.0f / len);
  const bool moving = len > 0.05f;

  // Sprint only when actually pressing forward and not sneaking (Skyrim rule).
  const bool can_sprint = sprint && !crouch && fwd > 0.1f;

  intent->move = move;
  intent->gait = can_sprint ? character::CharacterGait::kSprint : character::CharacterGait::kRun;
  intent->crouch = crouch;
  if (jump) intent->jump = true;

  // In FP the mouse turns the body/look heading (StepCharacters accumulates it,
  // and the eye-anchored camera follows). In TP the body heading is decoupled:
  // the camera orbits via cam_yaw_ and the biped facing (owned by the actor)
  // turns toward movement, so a standing player does not spin with the mouse.
  if (tp) {
    cam_yaw_ = WrapPi(cam_yaw_ + yaw_delta);
    intent->look_yaw_delta = 0;
  } else {
    intent->look_yaw_delta = yaw_delta;
  }
  intent->look_pitch_delta = 0;  // camera pitch is driven directly (cam_pitch_)

  // Expose sneak for stealth gameplay / HUD.
  ctx_.sneaking = crouch;
  (void)moving;
}

void PlayerController::PublishCamera() {
  ecs::World& world = *ctx_.world;
  auto* out = world.Get<scene::CameraOutput>(camera_output_);
  if (!out || !out->valid) return;
  const scene::CameraView& v = out->view;
  const Vec3 forward = Rotate(v.orientation, Vec3{0, 0, -1});
  ctx_.walk_eye = v.position;
  ctx_.walk_target = v.position + forward;
  ctx_.cam_yaw = std::atan2(forward.x, -forward.z);  // forward(yaw) = (sin,0,-cos)
  eye_position_ = v.position;
  eye_orientation_ = v.orientation;
}

void PlayerController::Update(f32 dt, const InputState& input, const ActionState& actions,
                             bool allow, bool auto_walk_active, const Vec3& auto_move,
                             Vec3* out_feet) {
  if (!assembled_ || dt <= 0.0f) return;
  ecs::World& world = *ctx_.world;
  physics::PhysicsWorld& phys = *ctx_.physics;

  // Scroll zoom (may request a mode switch by flipping ctx_.third_person).
  if (allow && input.wheel != 0.0f) ApplyZoom(input.wheel);
  // Keep the rx view kind in lockstep with ctx_.third_person (C key / zoom).
  ReconcileViewMode();

  FillIntent(input, actions, allow, auto_walk_active, auto_move, dt);

  // Drive the camera orbit directly. Third person orbits freely in world space
  // (decoupled from the body heading); first person is anchor-locked so the eye
  // yaw follows the body. Pitch is driven from cam_pitch_ in both.
  auto* vm = world.Get<character::CharacterViewMode>(player_);
  if (auto* orbit = world.Get<scene::CameraOrbit>(player_)) {
    if (vm && vm->kind == character::CharacterViewKind::kThirdPerson) {
      orbit->space = scene::CameraOrbitSpace::kWorld;
      orbit->min_pitch = view_settings_.tp_pitch_min;
      orbit->max_pitch = view_settings_.tp_pitch_max;
      orbit->yaw = cam_yaw_;
    } else {
      orbit->space = scene::CameraOrbitSpace::kAnchor;
      orbit->min_pitch = -view_settings_.fp_pitch_limit;
      orbit->max_pitch = view_settings_.fp_pitch_limit;
      orbit->yaw = 0.0f;  // yaw comes from the anchor heading in first person
    }
    orbit->pitch = std::clamp(cam_pitch_, orbit->min_pitch, orbit->max_pitch);
  }

  // rx character + camera-rig pipeline (README staged order).
  character::StepCharacters(world, phys, dt);
  character::SyncCharacterCameraAnchors(world);
  scene::BuildCameraRigs(world, dt);
  scene::PrepareCameraRigConstraints(world, dt);
  character::AnswerCameraObstructions(world, phys);
  scene::ResolveCameraRigs(world, dt);
  scene::ResolveCameraStacks(world, dt);

  PublishCamera();

  // Feed the actor: feet position (skeleton placement), planar speed (gait blend)
  // and body facing. The biped mesh faces +Z, so its facing yaw uses the movement
  // direction directly (atan2(x, z)); it eases toward the target so the body
  // turns to face movement instead of snapping (Skyrim third-person behaviour).
  auto* state = world.Get<character::CharacterState>(player_);
  auto* tr = world.Get<scene::Transform>(player_);
  if (state && tr) {
    const Vec3 feet{tr->position[0], tr->position[1], tr->position[2]};
    const f32 planar =
        std::sqrt(state->velocity.x * state->velocity.x + state->velocity.z * state->velocity.z);
    const bool moving = planar > 0.15f;
    if (moving) {
      const f32 target = std::atan2(state->velocity.x, state->velocity.z);  // biped +Z faces move
      facing_yaw_ = WrapPi(facing_yaw_ + WrapPi(target - facing_yaw_) * std::min(1.0f, dt * 12.0f));
    }
    actors_.MovePlayer(feet, planar, facing_yaw_, moving, state->grounded);
    if (out_feet) *out_feet = feet;
  }
}

}  // namespace rx
