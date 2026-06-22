#ifndef RECREATION_WORLD_NPC_AI_H_
#define RECREATION_WORLD_NPC_AI_H_

#include <cmath>

namespace rec::world {

// Tuning for planar (XZ) steering. +Y is up and never touched here; an NPC's
// height comes from physics/ground, not from these horizontal decisions.
struct SteerParams {
  float speed = 3.0f;         // cruise speed in meters/second
  float arrive_radius = 2.5f;  // begin slowing once within this of the goal
  float stop_radius = 1.4f;    // treat as arrived (stand still) within this
};

// Desired motion for one tick. `yaw` matches WalkUpdate's convention so the
// biped's +Z faces the way it moves. When standing, yaw is left at 0.
struct SteerOutput {
  float velocity[3] = {0, 0, 0};  // world-space, y component always 0
  float yaw = 0;                  // atan2(velocity.x, velocity.z), radians
  bool arrived = false;
};

// Seek `goal` from `pos` on the ground plane with arrival: cruise at full speed
// far out, slow linearly once inside `arrive_radius`, and stop dead inside
// `stop_radius` so followers do not jitter against the player. Vertical distance
// is ignored; the result keeps velocity.y at 0.
inline SteerOutput SteerToward(const float pos[3], const float goal[3], const SteerParams& p) {
  SteerOutput out;
  const float dx = goal[0] - pos[0];
  const float dz = goal[2] - pos[2];
  const float dist = std::sqrt(dx * dx + dz * dz);
  if (dist <= p.stop_radius) {
    out.arrived = true;
    return out;
  }
  float mag = p.speed;
  if (dist < p.arrive_radius) mag *= dist / p.arrive_radius;  // linear ramp-down
  const float inv = 1.0f / dist;
  out.velocity[0] = dx * inv * mag;
  out.velocity[2] = dz * inv * mag;
  out.yaw = std::atan2(out.velocity[0], out.velocity[2]);
  return out;
}

// Goal position for follower `slot` so a group forms up behind a leader instead
// of piling onto one point. Slot 0 sits directly behind at `spacing`; later
// slots fan out in a staggered V, alternating right/left and stepping farther
// back each pair. leader_yaw uses the WalkUpdate convention (forward =
// {sin,0,cos}); "behind" is the negated forward. Keeps the leader's height.
inline void FollowSlot(const float leader_pos[3], float leader_yaw, int slot, float spacing,
                       float out_goal[3]) {
  const float fx = std::sin(leader_yaw);
  const float fz = std::cos(leader_yaw);
  const float rx = fz;   // right = forward rotated -90 deg on XZ
  const float rz = -fx;
  const int pair = (slot + 1) / 2;             // 0,1,1,2,2,...
  const float side = (slot % 2 == 1) ? 1.0f : -1.0f;  // odd right, even left
  const float back = spacing * (1.0f + pair);  // step back each pair
  const float lateral = (slot == 0) ? 0.0f : 0.8f * spacing * static_cast<float>(pair);
  out_goal[0] = leader_pos[0] - fx * back + rx * side * lateral;
  out_goal[1] = leader_pos[1];
  out_goal[2] = leader_pos[2] - fz * back + rz * side * lateral;
}

// Sum of normalized push-away directions from neighbors within `radius` on the
// XZ plane, so co-located followers spread out. `others` is other_count*3 floats
// (xyz per neighbor). Writes a planar offset (y=0); zero when nothing is close.
// On exact overlap the push is deterministic (+X), mirroring ShoveOutOfRadius.
inline void SeparationOffset(const float self_pos[3], const float* others, int other_count,
                             float radius, float out_offset[3]) {
  out_offset[0] = 0;
  out_offset[1] = 0;
  out_offset[2] = 0;
  for (int i = 0; i < other_count; ++i) {
    const float dx = self_pos[0] - others[i * 3 + 0];
    const float dz = self_pos[2] - others[i * 3 + 2];
    const float dist = std::sqrt(dx * dx + dz * dz);
    if (dist >= radius) continue;
    if (dist <= 0.0f) {
      out_offset[0] += 1.0f;  // exact overlap: agree on a fixed direction
      continue;
    }
    const float inv = 1.0f / dist;
    out_offset[0] += dx * inv;
    out_offset[2] += dz * inv;
  }
}

}  // namespace rec::world

#endif  // RECREATION_WORLD_NPC_AI_H_
