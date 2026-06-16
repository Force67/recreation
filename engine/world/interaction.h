#ifndef RECREATION_WORLD_INTERACTION_H_
#define RECREATION_WORLD_INTERACTION_H_

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
    const float inv = 1.0f / __builtin_sqrtf(dist_sq);
    const float dot = (dx * player_fwd[0] + dy * player_fwd[1] + dz * player_fwd[2]) * inv;
    if (dot >= best_dot) {
      best_dot = dot;
      best = i;
    }
  }
  return best;
}

}  // namespace rec::world

#endif  // RECREATION_WORLD_INTERACTION_H_
