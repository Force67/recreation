// carriage_rigtest: exercises the horse-drawn carriage physics glue
// (world/carriage_rig) headless, with no Skyrim data. A flat ground, a scripted
// horse hitch point moved along a route, and the free-rolling rx vehicle towed
// behind it. Asserts the carriage (a) is towed forward, (b) tracks a turning
// route without diverging / jackknifing, (c) settles and parking-brakes when
// the horse stops, and (d) stays NaN-free over minutes of stepping.

#include <cmath>
#include <cstdio>

#include "physics/physics_world.h"
#include "world/carriage_rig.h"

using rx::f32;
using rx::Length;
using rx::Normalize;
using rx::Quat;
using rx::Rotate;
using rx::Vec3;
using rx::physics::PhysicsWorld;
using rx::world::CarriageConfig;
using rx::world::CarriageRig;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

bool Finite(const Vec3& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Angle (radians) between the chassis forward axis and the shaft direction
// (tongue -> horse), on the ground plane. Stays small while the cart trails the
// horse; a jackknife would swing it past 90 degrees.
f32 TrackingAngle(const CarriageRig& rig, const PhysicsWorld& world, const Vec3& horse) {
  Vec3 pos;
  f32 rot[4];
  if (!rig.Pose(world, &pos, rot)) return 0;
  const Quat q{rot[0], rot[1], rot[2], rot[3]};
  const Vec3 fwd = Rotate(q, Vec3{0, 0, 1});
  const Vec3 shaft = horse - rig.TonguePoint(world);
  const Vec3 fn = Normalize(Vec3{fwd.x, 0, fwd.z});
  const Vec3 sn = Normalize(Vec3{shaft.x, 0, shaft.z});
  f32 d = fn.x * sn.x + fn.z * sn.z;
  d = d > 1.0f ? 1.0f : (d < -1.0f ? -1.0f : d);
  return std::acos(d);
}

}  // namespace

int main() {
  std::puts("carriage rig:");

  PhysicsWorld world;
  if (!world.Initialize()) {
    // No Jolt in this build: the glue can't be exercised, so pass trivially the
    // way the runtime guards on physics_.initialized().
    std::puts("  [skip] physics unavailable (RX_JOLT off) -- nothing to test");
    return 0;
  }

  // A large flat ground slab with its top at y = 0.
  world.AddStaticBox({0, -2.0f, 0}, {80, 2, 80});

  const f32 dt = 1.0f / 60.0f;
  CarriageConfig cfg;
  CarriageRig rig;
  // Spawn a little high so the suspension settles onto the ground.
  const bool spawned = rig.Spawn(world, {0, 1.3f, 0}, 0.0f, cfg);
  Check("free-rolling carriage spawned", spawned);
  if (!spawned) return 1;

  bool all_finite = true;
  auto step = [&](const Vec3& horse, const Vec3& horse_vel) {
    rig.Step(world, horse, horse_vel, dt);
    world.Update(dt);
    Vec3 pos;
    f32 rot[4];
    rig.Pose(world, &pos, rot);
    if (!Finite(pos) || !std::isfinite(rot[0]) || !std::isfinite(rot[3])) all_finite = false;
    if (!Finite(world.GetLinearVelocity(rig.body()))) all_finite = false;
  };

  // The horse hitch point. Start it a rest-length ahead of the tongue and hold
  // it still while the cart drops onto its wheels; then read the settled tongue
  // height so the scripted route keeps the shaft horizontal.
  f32 hitch_y = 1.4f;
  Vec3 horse{0, hitch_y, cfg.tongue_z + cfg.rest_length};
  for (int i = 0; i < 150; ++i) step(horse, {0, 0, 0});
  hitch_y = rig.TonguePoint(world).y;
  horse.y = hitch_y;

  Vec3 settle_pos;
  f32 settle_rot[4];
  rig.Pose(world, &settle_pos, settle_rot);
  const Vec3 up_after_settle = Rotate(
      Quat{settle_rot[0], settle_rot[1], settle_rot[2], settle_rot[3]}, Vec3{0, 1, 0});
  Check("upright after settling", up_after_settle.y > 0.7f);

  // (a) Tow straight down +Z at a trot for 4 s.
  const f32 trot = 3.0f;
  const f32 z_before = settle_pos.z;
  for (int i = 0; i < 240; ++i) {
    horse.z += trot * dt;
    horse.y = hitch_y;
    step(horse, {0, 0, trot});
  }
  rig.Pose(world, &settle_pos, settle_rot);
  Check("towed forward", settle_pos.z - z_before > 6.0f);
  Check("trails behind the horse (still hitched)",
        Length(horse - rig.TonguePoint(world)) < cfg.rest_length + 1.5f);

  // (b) A sustained left turn: sweep the horse around a circle for ~8 s. The
  // cart must track it -- the tongue stays hitched (bounded distance) and the
  // chassis never swings past the shaft (no jackknife) -- and never flips.
  f32 heading = 0.0f;               // current horse heading, radians (0 = +Z)
  const f32 turn_radius = 7.0f;     // m
  const f32 turn_rate = trot / turn_radius;  // rad/s along the arc
  f32 max_track_angle = 0.0f;
  f32 max_hitch_dist = 0.0f;
  f32 min_up = 1.0f;
  for (int i = 0; i < 480; ++i) {
    heading += turn_rate * dt;
    const Vec3 vel{std::sin(heading) * trot, 0, std::cos(heading) * trot};
    horse.x += vel.x * dt;
    horse.z += vel.z * dt;
    horse.y = hitch_y;
    step(horse, vel);
    max_track_angle = std::max(max_track_angle, TrackingAngle(rig, world, horse));
    max_hitch_dist = std::max(max_hitch_dist, Length(horse - rig.TonguePoint(world)));
    Vec3 p;
    f32 r[4];
    rig.Pose(world, &p, r);
    min_up = std::min(min_up, Rotate(Quat{r[0], r[1], r[2], r[3]}, Vec3{0, 1, 0}).y);
  }
  std::printf("    turn: max track angle %.1f deg, max hitch dist %.2f m, min up %.2f\n",
              max_track_angle * 57.2958f, max_hitch_dist, min_up);
  Check("tracks the turn without jackknifing (< 70 deg)", max_track_angle < 1.22f);
  Check("stays hitched through the turn (< rest + 2 m)",
        max_hitch_dist < cfg.rest_length + 2.0f);
  Check("does not flip in the turn", min_up > 0.5f);

  // (c) Horse stops: the cart must coast to rest and hold (parking brake in).
  for (int i = 0; i < 240; ++i) step(horse, {0, 0, 0});
  const f32 rest_speed = Length(world.GetLinearVelocity(rig.body()));
  std::printf("    settled speed %.3f m/s, handbrake %.2f\n", rest_speed, rig.handbrake());
  Check("settles when the horse stops (< 0.35 m/s)", rest_speed < 0.35f);
  Check("parking brake engaged at rest", rig.handbrake() > 0.9f);

  // (d) Long run: keep stepping a wandering route and confirm no NaNs crept in.
  f32 t = 0;
  for (int i = 0; i < 3600; ++i) {  // 60 s
    t += dt;
    const Vec3 vel{std::sin(t) * 1.5f, 0, 2.0f};
    horse.x += vel.x * dt;
    horse.z += vel.z * dt;
    horse.y = hitch_y;
    step(horse, vel);
  }
  Check("NaN-free over the full run", all_finite);

  std::printf("%s\n", g_failures == 0 ? "carriage rig: PASS" : "carriage rig: FAIL");
  return g_failures == 0 ? 0 : 1;
}
