#ifndef RECREATION_WORLD_INTERACTION_H_
#define RECREATION_WORLD_INTERACTION_H_

#include <cmath>
#include <cstdint>

namespace rec::world {

// A thing the player could activate: its form handle (packed GlobalFormId) and
// world position. Kept free of engine types so the picker is trivially testable.
struct ActivationCandidate {
  uint64_t form_handle = 0;
  float pos[3] = {0, 0, 0};
};

// Picks the candidate the player is looking at to activate, the Skyrim "press E"
// target. A candidate qualifies when it is within `max_range` and inside the
// view cone (the normalized direction to it dotted with `player_fwd` is at least
// `min_facing_dot`); among those the most centered (largest facing dot) wins,
// matching how the game favors what is dead ahead over what is merely near.
// Returns the index of the best candidate, or -1 when none qualifies.
//
// `player_fwd` must be normalized. Heights are included so the player cannot
// activate something far above or below; pass an eye-height position.
inline int PickActivationTarget(const float player_pos[3], const float player_fwd[3],
                                const ActivationCandidate* candidates, int count, float max_range,
                                float min_facing_dot) {
  int best = -1;
  float best_dot = min_facing_dot;
  const float max_range_sq = max_range * max_range;
  for (int i = 0; i < count; ++i) {
    const float dx = candidates[i].pos[0] - player_pos[0];
    const float dy = candidates[i].pos[1] - player_pos[1];
    const float dz = candidates[i].pos[2] - player_pos[2];
    const float dist_sq = dx * dx + dy * dy + dz * dz;
    if (dist_sq > max_range_sq || dist_sq <= 0.0f) continue;
    const float inv = 1.0f / std::sqrt(dist_sq);
    const float dot = (dx * player_fwd[0] + dy * player_fwd[1] + dz * player_fwd[2]) * inv;
    if (dot >= best_dot) {
      best_dot = dot;
      best = i;
    }
  }
  return best;
}

// Server-authoritative "shove": if `target` sits inside a vertical cylinder of
// `radius` around `pusher` on the horizontal (XZ) plane, writes the position it
// is pushed out to (cylinder edge, height unchanged) into `out` and returns
// true; returns false when it is already clear. A degenerate exact overlap is
// shoved along +X so the result is deterministic. Engine space, +Y up.
//
// A simple stand-in for full character physics: it lets a player nudge an NPC
// out of the way, and because it only moves the NPC's transform the existing
// actor-sync replication carries the result to every client.
inline bool ShoveOutOfRadius(const float pusher[3], const float target[3], float radius,
                             float out[3]) {
  const float dx = target[0] - pusher[0];
  const float dz = target[2] - pusher[2];
  const float dist_sq = dx * dx + dz * dz;
  if (dist_sq >= radius * radius) return false;
  out[1] = target[1];
  const float dist = std::sqrt(dist_sq);
  if (dist < 1e-4f) {  // exactly on top of the pusher: pick a fixed direction
    out[0] = pusher[0] + radius;
    out[2] = pusher[2];
    return true;
  }
  const float scale = radius / dist;
  out[0] = pusher[0] + dx * scale;
  out[2] = pusher[2] + dz * scale;
  return true;
}

}  // namespace rec::world

#endif  // RECREATION_WORLD_INTERACTION_H_
