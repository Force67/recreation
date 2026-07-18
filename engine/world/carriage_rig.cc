#include "world/carriage_rig.h"

#include <algorithm>
#include <cmath>

namespace rx::world {

bool CarriageRig::Spawn(physics::PhysicsWorld& world, const Vec3& position, f32 yaw,
                        const CarriageConfig& cfg) {
  cfg_ = cfg;
  physics::PhysicsWorld::VehicleDesc desc;
  desc.half_extent = cfg.half_extent;
  desc.mass = cfg.mass;
  desc.wheel_radius = cfg.wheel_radius;
  desc.wheel_x = cfg.wheel_x;
  desc.front_z = cfg.front_z;
  desc.rear_z = cfg.rear_z;
  desc.max_steer_angle = cfg.max_steer_angle;
  // Unpowered: the hitch does all the pulling. free_rolling drops the
  // differential so no engine torque reaches any wheel and they coast.
  desc.max_engine_torque = 0;
  desc.free_rolling = true;
  desc.max_handbrake_torque = cfg.handbrake_torque;
  vehicle_ = world.CreateVehicle(desc, position, yaw);
  if (!vehicle_) return false;
  body_ = world.GetVehicleBody(vehicle_);
  return body_ != 0;
}

bool CarriageRig::Pose(const physics::PhysicsWorld& world, Vec3* position,
                       f32 rotation[4]) const {
  if (!vehicle_) return false;
  return world.GetVehicleTransform(vehicle_, position, rotation);
}

Vec3 CarriageRig::TonguePoint(const physics::PhysicsWorld& world) const {
  Vec3 pos;
  f32 rot[4];
  if (!world.GetVehicleTransform(vehicle_, &pos, rot)) return {};
  const Quat q{rot[0], rot[1], rot[2], rot[3]};
  return pos + Rotate(q, Vec3{0, cfg_.tongue_y, cfg_.tongue_z});
}

void CarriageRig::Step(physics::PhysicsWorld& world, const Vec3& hitch_target,
                       const Vec3& horse_velocity, f32 dt) {
  (void)dt;
  if (!vehicle_ || !body_) return;

  Vec3 pos;
  f32 rot[4];
  if (!world.GetVehicleTransform(vehicle_, &pos, rot)) return;
  const Quat q{rot[0], rot[1], rot[2], rot[3]};
  const Vec3 fwd = Rotate(q, Vec3{0, 0, 1});

  // Tongue point and its world velocity (chassis linear + angular contribution).
  const Vec3 tongue = pos + Rotate(q, Vec3{0, cfg_.tongue_y, cfg_.tongue_z});
  const Vec3 tongue_vel = world.GetPointVelocity(body_, tongue);

  // Spring-damper tow along the shaft. `stretch` is signed about rest_length so
  // the rigid shaft pulls when the horse is ahead and pushes when the cart
  // coasts into a stopped horse; the damper acts on the closing speed.
  Vec3 to_hitch = hitch_target - tongue;
  const f32 dist = Length(to_hitch);
  const Vec3 dir = dist > 1e-4f ? to_hitch * (1.0f / dist) : fwd;
  const f32 spring = cfg_.hitch_stiffness * (dist - cfg_.rest_length);
  const f32 damp = cfg_.hitch_damping * Dot(horse_velocity - tongue_vel, dir);
  f32 mag = spring + damp;
  mag = std::clamp(mag, -cfg_.max_hitch_force, cfg_.max_hitch_force);
  world.AddForceAtPoint(body_, dir * mag, tongue);

  // Turntable steering: signed ground-plane yaw of the pull direction relative
  // to the chassis forward axis. (fwd x dir).y is negative when the target lies
  // to the right (-X), and rx's steer input is +right, hence the negation.
  const Vec3 flat_fwd{fwd.x, 0, fwd.z};
  const Vec3 flat_dir{dir.x, 0, dir.z};
  f32 steer = 0;
  if (Length(flat_fwd) > 1e-3f && Length(flat_dir) > 1e-3f) {
    const Vec3 fn = Normalize(flat_fwd);
    const Vec3 dn = Normalize(flat_dir);
    const f32 cross_y = fn.z * dn.x - fn.x * dn.z;  // (fwd x dir).y
    const f32 yaw_err = std::atan2(cross_y, Dot(fn, dn));
    steer = std::clamp(-yaw_err * cfg_.steer_gain, -1.0f, 1.0f);
  }
  steer_ = steer;

  // Parking brake ramps in as the horse slows so the cart settles and holds.
  const f32 horse_speed = Length(Vec3{horse_velocity.x, 0, horse_velocity.z});
  handbrake_ = horse_speed < cfg_.park_speed
                   ? 1.0f - horse_speed / cfg_.park_speed
                   : 0.0f;

  physics::PhysicsWorld::VehicleInput input;
  input.throttle = 0;
  input.steer = steer_;
  input.brake = 0;
  input.handbrake = handbrake_;
  world.DriveVehicle(vehicle_, input);
}

}  // namespace rx::world
