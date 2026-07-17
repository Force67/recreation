#ifndef RECREATION_RUNTIME_PLAYER_CONTROLLER_H_
#define RECREATION_RUNTIME_PLAYER_CONTROLLER_H_

#include <unordered_map>

#include "bethesda/movement_type.h"
#include "character/character.h"
#include "core/math.h"
#include "ecs/entity.h"

namespace rx {

struct EngineContext;
class ActorSystem;
class InputMap;
struct InputState;
struct ActionState;

// Skyrim-authentic player locomotion + first/third-person camera, built on the
// rx::character capsule controller and rx::scene camera rig. It owns the tuning
// tables (decoded from MOVT/GMST where possible, documented SE defaults
// otherwise), assembles the character + camera-rig components onto the player
// actor entity, fills CharacterIntent from the game's Action/Axis layer each
// frame, runs the staged character/camera pipeline and lands the result where
// the renderer already reads it (EngineContext walk_eye/walk_target/cam_yaw).
//
// The actor system still owns the biped skeleton: after the capsule steps, the
// controller hands the actor the feet position (skeleton placement), planar
// speed (gait blend), body facing and grounded state via ActorSystem::MovePlayer.
class PlayerController {
 public:
  PlayerController(EngineContext& ctx, ActorSystem& actors, const InputMap& input_map);
  ~PlayerController();

  // Adds the rx character + camera components to the player actor entity and
  // creates the camera-stack output. Safe to call once the player actor exists;
  // returns false (and is a no-op) if there is no player yet. Idempotent guard.
  bool Assemble();
  bool assembled() const { return assembled_; }

  // One frame of walk-mode player control. `allow` gates keyboard/mouse (menus /
  // typing steal it). `auto_move` (when auto_walk_active) overrides the movement
  // direction (world-space, unit length) for quest-guided auto-walk. Fills
  // intent, runs the pipeline, writes ctx_.walk_eye/walk_target/cam_yaw and feeds
  // the actor. `out_feet` receives the player feet position (melee / battle cam).
  void Update(f32 dt, const InputState& input, const ActionState& actions, bool allow,
              bool auto_walk_active, const Vec3& auto_move, Vec3* out_feet);

  // The camera-space eye transform (world position + orientation) the first-person
  // equipment layer attaches arms to. Valid after the first Update.
  const Vec3& eye_position() const { return eye_position_; }
  const Quat& eye_orientation() const { return eye_orientation_; }

 private:
  void FillIntent(const InputState& input, const ActionState& actions, bool allow,
                  bool auto_walk_active, const Vec3& auto_move, f32 dt);
  void ReconcileViewMode();  // sync rx view kind to ctx_.third_person (smooth toggle)
  void ApplyZoom(f32 wheel);
  void PublishCamera();

  EngineContext& ctx_;
  ActorSystem& actors_;
  const InputMap& input_map_;

  bool assembled_ = false;
  ecs::Entity player_{};          // the player actor entity (rx components live here)
  ecs::Entity camera_output_{};   // CameraOutput sink read back each frame

  character::CharacterViewSettings view_settings_;
  std::unordered_map<u64, bethesda::MovementType> movement_types_;

  // Third-person camera yaw is independent of the body heading (Skyrim: standing
  // still the camera orbits without turning the body). First person drives the
  // body/look heading directly through the character intent. Pitch is shared.
  f32 cam_yaw_ = 0;      // third-person free-orbit yaw (radians)
  f32 cam_pitch_ = 0;    // camera pitch, both modes (radians)
  f32 loco_debug_t_ = 0; // RX_LOCO_DEBUG trace throttle accumulator (seconds)
  bool was_third_person_ = true;

  Vec3 eye_position_{};
  Quat eye_orientation_{0, 0, 0, 1};
};

}  // namespace rx

#endif  // RECREATION_RUNTIME_PLAYER_CONTROLLER_H_
