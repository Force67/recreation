#ifndef RECREATION_WORLD_OBJECTIVE_MARKER_H_
#define RECREATION_WORLD_OBJECTIVE_MARKER_H_

#include <cmath>

namespace rec::world {

// Pure geometry for objective waypoints, engine-type free so it is trivially
// testable. The world is +Y up; markers sit on the ground, so the tests are
// horizontal (XZ) and ignore height, the same way activation reach does.

// True when `player` is within `radius` of `marker` on the XZ plane. Height is
// ignored: a marker on a floor still triggers for a player whose feet sit a
// little above or below it, but the XZ distance is respected.
inline bool MarkerReached(const float player[3], const float marker[3], float radius) {
  const float dx = player[0] - marker[0];
  const float dz = player[2] - marker[2];
  return dx * dx + dz * dz <= radius * radius;
}

// Compass bearing from where the player looks to the marker, in degrees: 0 is
// dead ahead, positive is to the player's right, +/-180 is behind. Both inputs
// are XZ directions (y ignored) and need not be normalized. Returns 0 when
// either is degenerate. The sign matches the HUD compass, whose pip slides
// right for a positive bearing.
inline float MarkerCompassBearingDeg(const float view_fwd[3], const float to_marker[3]) {
  float fx = view_fwd[0], fz = view_fwd[2];
  float tx = to_marker[0], tz = to_marker[2];
  const float fl = __builtin_sqrtf(fx * fx + fz * fz);
  const float tl = __builtin_sqrtf(tx * tx + tz * tz);
  if (fl < 1e-6f || tl < 1e-6f) return 0.0f;
  fx /= fl;
  fz /= fl;
  tx /= tl;
  tz /= tl;
  const float det = fz * tx - fx * tz;  // > 0 when the marker is to the right
  const float dot = fx * tx + fz * tz;
  return std::atan2(det, dot) * 57.2957795f;
}

}  // namespace rec::world

#endif  // RECREATION_WORLD_OBJECTIVE_MARKER_H_
