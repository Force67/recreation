#ifndef RECREATION_RUNTIME_CARRIAGE_H_
#define RECREATION_RUNTIME_CARRIAGE_H_

#include <string>

#include <base/containers/vector.h>

#include "bethesda/record.h"
#include "core/math.h"
#include "ecs/world.h"
#include "world/carriage_rig.h"

namespace rx {

struct EngineContext;
class ActorSystem;

// The horse-drawn carriage gameplay slice (dev spawn: RX_CARRIAGE=1). A horse,
// simulated as a host-authoritative kinematic mover along a looping route, pulls
// a free-rolling physics carriage (world::CarriageRig) through a spring-damper
// hitch. Activating the carriage seats the player as a passenger (locomotion
// off, camera on the seat); activating again dismounts. Host-authoritative:
// the carriage/horse are host-side entities, so in multiplayer they are visible
// on the host only (they carry no form provenance to ride the actor-sync path).
//
// Content: the carriage body renders from the Skyrim carriage NIF when the data
// is mounted (else a graybox box), and the wheels are engine-drawn cylinders at
// the physics wheel transforms; the horse uses the creature rig (else a graybox
// body). Nothing here requires game data — with none present the whole rig is
// graybox and still simulates.
class CarriageSystem {
 public:
  CarriageSystem(EngineContext& ctx, ActorSystem* actors);

  // Host-authoritative fixed step, staged BEFORE physics (kPreSim): advances the
  // horse along the route and tows/steers the carriage. Lazily spawns the rig on
  // the first call when RX_CARRIAGE is set and physics is up.
  void Step(f32 dt);
  // Render sync after physics (kPostSim): wheel + horse transforms.
  void SyncRender();

  // Ride flow.
  bool riding() const { return riding_; }
  // Frame-cadence ride update: pins the player to the seat and frames the camera
  // on it while seated.
  void UpdateRide(f32 dt);
  // kActivateRef handler: boards or dismounts if `handle` is the carriage.
  // Returns true when it owned the handle.
  bool Activate(u64 handle);
  // Activation prompt for the carriage handle, or null when not the carriage.
  const char* Label(u64 handle) const;

 private:
  void Spawn(const Vec3& origin);
  Vec3 RoutePoint(f32 arc) const;          // world point on the loop at arc length
  Vec3 RouteTangent(f32 arc) const;        // unit travel direction at arc length
  f32 GroundY(f32 x, f32 z, f32 y_hint) const;
  Vec3 SeatWorld() const;                  // world seat pose from the chassis

  EngineContext& ctx_;
  ActorSystem* actors_;
  bool enabled_ = false;   // RX_CARRIAGE set
  bool spawned_ = false;

  world::CarriageRig rig_;
  ecs::Entity body_entity_{};              // carriage chassis (mesh, mirrored pose)
  ecs::Entity wheel_entity_[4]{};          // engine-drawn wheels
  ecs::Entity horse_entity_{};             // kinematic mover
  bool horse_is_rig_ = false;              // creature rig vs graybox horse

  // Route: a level loop the horse walks. Centre/radius are set at spawn.
  Vec3 route_center_{};
  f32 route_radius_ = 14.0f;
  f32 route_arc_ = 0;      // horse position along the loop, metres
  f32 trot_speed_ = 3.0f;  // m/s

  // Where the tongue hitches on the horse: behind the horse body, at hitch
  // height. Kept level with the carriage tongue so the shaft stays horizontal.
  f32 horse_hitch_back_ = 1.1f;
  Vec3 prev_hitch_{};      // last hitch point, for the horse hitch velocity

  bool riding_ = false;
  bethesda::GlobalFormId carriage_form_{};  // synthetic, for activation picking
  u64 carriage_handle_ = 0;
  std::string label_;
};

}  // namespace rx

#endif  // RECREATION_RUNTIME_CARRIAGE_H_
