#ifndef RECREATION_WORLD_CARRIAGE_RIG_H_
#define RECREATION_WORLD_CARRIAGE_RIG_H_

#include "core/math.h"
#include "physics/physics_world.h"

namespace rx::world {

// Physics model for a horse-drawn carriage: a free-rolling four-wheel chassis
// (an unpowered rx VehicleDesc: no differential, so the wheels coast on their
// suspension and tire grip) that is pulled through a spring-damper hitch at the
// tongue, with a turntable front axle steered toward the pull the way a
// real cart's front bolster tracks the shafts. SI units, engine space
// (+Z forward, +Y up, right-handed, so the RIGHT side is -X).
//
// This is the pure physics glue: it owns the rx vehicle and stages the tow
// force / steer / parking-brake each fixed step from a horse hitch point the
// caller supplies. It has no ECS, rendering or content dependency, so the
// physics can be exercised headless without game data.
struct CarriageConfig {
  // Chassis geometry, forwarded to the rx VehicleDesc. A light wooden cart.
  Vec3 half_extent{0.85f, 0.45f, 1.5f};
  f32 mass = 350;              // kg
  f32 wheel_radius = 0.5f;     // tall cart wheels
  f32 wheel_x = 0.9f;          // half track (wheel centre from chassis centre)
  f32 front_z = 1.25f;         // axle offsets, +Z forward
  f32 rear_z = -1.25f;
  f32 max_steer_angle = 0.7f;  // rad; the turntable front-axle range
  f32 handbrake_torque = 4000; // Nm on the rear axle; the parking brake

  // Hitch. The tongue attach point sits `tongue_z` ahead of the chassis centre
  // and `tongue_y` above it; a stiff spring-damper pulls it toward the horse's
  // hitch point and holds a `rest_length` gap. The shaft both pulls (horse
  // ahead) and pushes (cart coasting into a stopped horse). `hitch_damping`
  // acts along the shaft on the tongue-vs-horse closing speed; `max_hitch_force`
  // clamps the explicit spring so a large stretch can't blow up the 60 Hz step.
  f32 tongue_z = 1.9f;
  f32 tongue_y = 0.1f;
  f32 rest_length = 1.4f;       // m, tongue -> horse hitch
  f32 hitch_stiffness = 24000;  // N/m
  f32 hitch_damping = 5500;     // N/(m/s)
  f32 max_hitch_force = 60000;  // N

  // Steering. The front axle turns toward the hitch direction: the signed yaw
  // between the chassis forward axis and the tongue->horse direction (ground
  // plane) is scaled by `steer_gain` onto the -1..1 steer input and saturated,
  // so the cart tracks the horse through a turn without jackknifing but a sharp
  // pull just holds full lock instead of oscillating.
  f32 steer_gain = 3.0f;

  // Parking. Below `park_speed` (horse hitch speed, m/s) the handbrake ramps in
  // so a coasting cart settles and holds on a slope instead of rolling on.
  f32 park_speed = 0.35f;
};

class CarriageRig {
 public:
  // Spawns the free-rolling chassis at `position` (chassis centre) yawed about
  // +Y. Spawn slightly high and let the suspension settle. Returns false if the
  // physics world could not create the vehicle.
  bool Spawn(physics::PhysicsWorld& world, const Vec3& position, f32 yaw,
             const CarriageConfig& cfg);
  bool valid() const { return vehicle_ != 0; }

  // One fixed step, staged BEFORE physics::PhysicsWorld::Update (the rx vehicle
  // update-ordering contract). Applies the tow force toward `hitch_target` (the
  // horse's hitch point, world space) given the horse moving at
  // `horse_velocity`, steers the front axle toward it, and engages the parking
  // brake as the horse slows. `dt` is the world fixed step.
  void Step(physics::PhysicsWorld& world, const Vec3& hitch_target,
            const Vec3& horse_velocity, f32 dt);

  physics::VehicleId vehicle() const { return vehicle_; }
  physics::BodyId body() const { return body_; }
  const CarriageConfig& config() const { return cfg_; }

  // Chassis pose (position + quaternion x,y,z,w); false before a successful
  // Spawn.
  bool Pose(const physics::PhysicsWorld& world, Vec3* position, f32 rotation[4]) const;
  // World-space tongue (hitch attach) point for the current chassis pose.
  Vec3 TonguePoint(const physics::PhysicsWorld& world) const;
  // Last steer input (-1..1) and parking-brake amount (0..1) staged by Step,
  // for HUD / audio / debug.
  f32 steer_input() const { return steer_; }
  f32 handbrake() const { return handbrake_; }

 private:
  CarriageConfig cfg_;
  physics::VehicleId vehicle_ = 0;
  physics::BodyId body_ = 0;
  f32 steer_ = 0;
  f32 handbrake_ = 0;
};

}  // namespace rx::world

#endif  // RECREATION_WORLD_CARRIAGE_RIG_H_
