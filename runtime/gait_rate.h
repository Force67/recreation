#ifndef RECREATION_RUNTIME_GAIT_RATE_H_
#define RECREATION_RUNTIME_GAIT_RATE_H_

#include <algorithm>

#include "core/types.h"

namespace rx {

// Anti foot-slide: the playback rate for the walk<->run locomotion phase clock
// that keeps the DOMINANT gait clip planted against the ground.
//
// The locomotion state machine drives a walk<->run blend space off actual planar
// speed, so across the authored [walk_speed, run_speed] range the blended pose is
// already authored for that speed and the natural clock (speed / walk_speed) plants
// the feet correctly -- this function is the IDENTITY there (returns speed/walk).
//
// Feet slide only when the actual speed sits OFF the authored clips: below walk
// (a slow analog / speed-blend shuffle over-steps) or above run (sprint clamps to
// the run clip, whose stride is too short for the faster ground). There this
// applies the mission's clamped correction -- rate = actual / authored_reference,
// clamped to [0.7, 1.4] to avoid chipmunk / slow-mo -- so the clip's stride cadence
// tracks the ground instead of sliding. Returned in "authored walk cadence" units
// (the leader play-rate the SyncGroup / phase clock expects).
inline f32 GaitPlaybackRate(f32 speed, f32 walk_speed, f32 run_speed) {
  const f32 w = std::max(walk_speed, 0.05f);
  const f32 r = std::max(run_speed, w + 0.1f);
  const f32 ref = std::clamp(speed, w, r);  // nearest authored speed within [walk, run]
  const f32 correction = std::clamp(speed / ref, 0.7f, 1.4f);  // ~1 inside the blend range
  return (ref / w) * correction;
}

}  // namespace rx

#endif  // RECREATION_RUNTIME_GAIT_RATE_H_
