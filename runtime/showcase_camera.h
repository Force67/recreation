#ifndef RECREATION_RUNTIME_SHOWCASE_CAMERA_H_
#define RECREATION_RUNTIME_SHOWCASE_CAMERA_H_

#include <string>

#include <base/containers/vector.h>

#include "core/math.h"

namespace rx {

struct ShowcasePose {
  Vec3 eye{};
  Vec3 target{};
};

// A scripted cinematic flythrough. The camera glides through a list of
// waypoints on a Catmull-Rom path, easing in and out of each (smoothstep) so it
// reads like a hand-flown drone rather than a robotic dolly. Velocity is ~zero
// at every waypoint, which both looks deliberate and keeps the marked capture
// frames sharp. Waypoints flagged `capture` are deterministic regression marks.
class ShowcaseCamera {
 public:
  struct Waypoint {
    Vec3 eye{};
    Vec3 look{};     // point the camera aims at
    f32 travel = 6;  // seconds to glide here from the previous waypoint
    bool capture = false;
    std::string label;
  };

  void Add(Waypoint wp);
  bool empty() const { return keys_.empty(); }
  size_t size() const { return keys_.size(); }
  f32 duration() const { return total_; }

  // Eased pose at absolute time t (clamped to [0, duration]).
  ShowcasePose Sample(f32 t) const;

  // If a capture waypoint's arrival falls in (prev_t, t], returns its index and
  // writes its label; otherwise returns -1.
  int CaptureCrossed(f32 prev_t, f32 t, std::string* label) const;

 private:
  base::Vector<Waypoint> keys_;
  base::Vector<f32> arrive_;  // absolute time the camera reaches keys_[i]
  f32 total_ = 0;
};

}  // namespace rx

#endif  // RECREATION_RUNTIME_SHOWCASE_CAMERA_H_
