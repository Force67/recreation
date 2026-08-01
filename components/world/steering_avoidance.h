#ifndef RECREATION_WORLD_STEERING_AVOIDANCE_H_
#define RECREATION_WORLD_STEERING_AVOIDANCE_H_

#include <cmath>

namespace rx::world {

// Context steering: choose a travel direction toward `goal_dir` that avoids
// nearby obstacles. `candidate_dirs` is `count` * 3 floats (xyz unit vectors on
// the XZ plane, e.g. the goal fanned left/right); `clearances[i]` is the open
// distance along candidate_dirs[i] (a large value = clear, ~0 = blocked).
// `danger_dist` is the distance under which an obstacle fully penalizes a
// candidate. Writes the chosen unit direction (XZ, y = 0) into `out_dir`.
//
// Scoring per candidate: an alignment term (how well it points at goal_dir,
// remapped from [-1,1] to [0,1]) times a clearance term (clearances[i] capped
// at danger_dist, divided by danger_dist, so a wall right ahead scores ~0).
// The highest score wins; this naturally deflects around a blocked goal toward
// an open, still goal-ish direction. With count <= 0, copies goal_dir through.
inline void SteerAroundObstacles(const float goal_dir[3], const float* candidate_dirs,
                                 const float* clearances, int count, float danger_dist,
                                 float out_dir[3]) {
  const float goal_len_sq = goal_dir[0] * goal_dir[0] + goal_dir[2] * goal_dir[2];
  if (count <= 0 || goal_len_sq <= 0.0f) {
    out_dir[0] = goal_dir[0];
    out_dir[1] = goal_dir[1];
    out_dir[2] = goal_dir[2];
    return;
  }
  const float inv = 1.0f / std::sqrt(goal_len_sq);
  const float gx = goal_dir[0] * inv;
  const float gz = goal_dir[2] * inv;

  int best = 0;
  float best_score = -1.0f;
  for (int i = 0; i < count; ++i) {
    const float* dir = candidate_dirs + i * 3;
    const float alignment = gx * dir[0] + gz * dir[2];
    const float alignment_remap = (alignment + 1.0f) * 0.5f;
    float clearance = 1.0f;
    if (danger_dist > 0.0f) {
      const float capped = clearances[i] < danger_dist ? clearances[i] : danger_dist;
      clearance = capped / danger_dist;
    }
    const float score = alignment_remap * clearance;
    if (score > best_score) {
      best_score = score;
      best = i;
    }
  }

  const float* chosen = candidate_dirs + best * 3;
  out_dir[0] = chosen[0];
  out_dir[1] = 0.0f;
  out_dir[2] = chosen[2];
}

}  // namespace rx::world

#endif  // RECREATION_WORLD_STEERING_AVOIDANCE_H_
