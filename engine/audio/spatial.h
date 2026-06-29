#ifndef RECREATION_AUDIO_SPATIAL_H_
#define RECREATION_AUDIO_SPATIAL_H_

#include <algorithm>
#include <cmath>

#include "core/math.h"
#include "core/types.h"

namespace rec::audio {

// The mixer's view of the player: where the ears are and which way they face, in
// engine space (Y-up, metres). Updated once per frame from the camera (or the
// walk-mode player) and read on the mixer thread, so it is a plain value the
// device snapshots under its lock.
struct Listener {
  Vec3 position{};
  Vec3 forward{0, 0, -1};
  Vec3 up{0, 1, 0};
};

// Distance falloff for a positional source, in engine metres. Inside `ref_dist`
// the sound is at full volume; beyond it the gain rolls off inverse-distance and
// is faded to silence by `max_dist`, so distant ambience does not leak in at a
// constant floor.
struct Attenuation {
  f32 ref_dist = 4.0f;
  f32 max_dist = 60.0f;
};

// Per-channel gains the mixer multiplies a mono/stereo source by.
struct StereoGains {
  f32 left = 1.0f;
  f32 right = 1.0f;
};

// Inverse-distance attenuation with a smooth cutoff at max_dist. Returns 1 at and
// within ref_dist, falling to 0 at max_dist.
inline f32 DistanceGain(f32 distance, const Attenuation& a) {
  if (distance <= a.ref_dist) return 1.0f;
  if (distance >= a.max_dist) return 0.0f;
  const f32 inv = a.ref_dist / distance;  // inverse-distance law
  // Fade the inverse curve to exactly zero over the last stretch so there is no
  // audible pop when a voice crosses max_dist.
  const f32 fade = (a.max_dist - distance) / std::max(0.001f, a.max_dist - a.ref_dist);
  return inv * std::clamp(fade, 0.0f, 1.0f);
}

// Constant-power stereo pan from the listener to a world-space source. The result
// folds in distance attenuation, so the mixer applies one gain pair per channel.
// `min_bleed` keeps a hard-panned source partly present in the far ear, which
// sounds far more natural than a fully silent channel.
inline StereoGains PanForSource(const Listener& listener, const Vec3& source,
                                const Attenuation& atten, f32 min_bleed = 0.18f) {
  const Vec3 to_source = source - listener.position;
  const f32 distance = Length(to_source);
  const f32 gain = DistanceGain(distance, atten);
  if (gain <= 0.0f) return {0.0f, 0.0f};

  // Listener basis. right = forward x up (left-handed engine convention used by
  // the camera); a degenerate direction (source on top of the listener) pans
  // centre.
  const Vec3 forward = Normalize(listener.forward);
  const Vec3 up = Normalize(listener.up);
  const Vec3 right = Normalize(Cross(forward, up));
  f32 pan = 0.0f;
  if (distance > 1e-4f) pan = std::clamp(Dot(to_source * (1.0f / distance), right), -1.0f, 1.0f);

  // Map pan [-1,1] -> angle [0, pi/2] and take cos/sin for equal-power gains.
  const f32 angle = (pan * 0.5f + 0.5f) * 1.57079633f;
  f32 l = std::cos(angle);
  f32 r = std::sin(angle);
  l = min_bleed + (1.0f - min_bleed) * l;
  r = min_bleed + (1.0f - min_bleed) * r;
  return {l * gain, r * gain};
}

}  // namespace rec::audio

#endif  // RECREATION_AUDIO_SPATIAL_H_
