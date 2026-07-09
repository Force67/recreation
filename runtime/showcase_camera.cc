#include "showcase_camera.h"

#include <algorithm>
#include <utility>

namespace rx {

void ShowcaseCamera::Add(Waypoint wp) {
  total_ = keys_.empty() ? 0.0f : total_ + std::max(0.0f, wp.travel);
  arrive_.push_back(total_);
  keys_.push_back(std::move(wp));
}

namespace {
// Uniform Catmull-Rom through p1..p2 with p0/p3 as the neighbouring marks.
Vec3 CatmullRom(const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3, f32 u) {
  f32 u2 = u * u;
  f32 u3 = u2 * u;
  return (p1 * 2.0f + (p2 - p0) * u + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * u2 +
          (p1 * 3.0f - p0 - p2 * 3.0f + p3) * u3) *
         0.5f;
}
}  // namespace

ShowcasePose ShowcaseCamera::Sample(f32 t) const {
  if (keys_.empty()) return {};
  if (keys_.size() == 1) return {keys_[0].eye, keys_[0].look};
  t = std::clamp(t, 0.0f, total_);

  // Segment [a, b] of the timeline that contains t.
  size_t b = 1;
  while (b < keys_.size() && arrive_[b] < t) ++b;
  size_t a = b - 1;
  f32 span = arrive_[b] - arrive_[a];
  f32 u = span > 1e-5f ? (t - arrive_[a]) / span : 0.0f;
  f32 e = u * u * (3.0f - 2.0f * u);  // smoothstep: ease in/out, zero velocity at marks

  size_t i0 = a > 0 ? a - 1 : a;
  size_t i3 = b + 1 < keys_.size() ? b + 1 : b;
  ShowcasePose pose;
  pose.eye = CatmullRom(keys_[i0].eye, keys_[a].eye, keys_[b].eye, keys_[i3].eye, e);
  pose.target = Lerp(keys_[a].look, keys_[b].look, e);
  return pose;
}

int ShowcaseCamera::CaptureCrossed(f32 prev_t, f32 t, std::string* label) const {
  for (size_t i = 0; i < keys_.size(); ++i) {
    if (!keys_[i].capture) continue;
    if (arrive_[i] > prev_t && arrive_[i] <= t) {
      if (label) *label = keys_[i].label;
      return static_cast<int>(i);
    }
  }
  return -1;
}

}  // namespace rx
