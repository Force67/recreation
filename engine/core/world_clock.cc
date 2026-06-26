#include "core/world_clock.h"

namespace rec {

namespace {

f32 Clamp01(f32 x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

f32 Smoothstep(f32 edge0, f32 edge1, f32 x) {
  const f32 t = Clamp01((x - edge0) / (edge1 - edge0));
  return t * t * (3.0f - 2.0f * t);
}

f32 LerpF(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

constexpr f32 kPi = 3.14159265358979323846f;

}  // namespace

SkyLighting ComputeSkyLighting(f32 hour) {
  // Sun arc: hour 6 -> 0 (sunrise, due east), 12 -> pi/2 (noon, zenith),
  // 18 -> pi (sunset, due west). A small constant south lean (the -0.25 z) keeps
  // shadows off the world axes for a more natural look.
  const f32 a = (hour - 6.0f) / 12.0f * kPi;
  const f32 ca = std::cos(a);
  const f32 elev = std::sin(a);  // sun elevation: > 0 day, < 0 night

  // Daylight weight, with a soft twilight band as the sun crosses the horizon.
  const f32 day = Smoothstep(-0.12f, 0.12f, elev);

  // Daytime sun: it travels opposite its sky position, so the light comes from
  // the sun. Warm orange near the horizon, neutral white overhead.
  const Vec3 day_dir = Normalize(Vec3{-ca, -elev, -0.25f});
  const f32 high = Clamp01(elev / 0.5f);
  const Vec3 day_color = Lerp(Vec3{1.0f, 0.50f, 0.26f}, Vec3{1.0f, 0.96f, 0.90f}, high);
  constexpr f32 kDayIntensity = 4.0f;

  // Night: a dim cool moon that always lights from above (never under-lighting
  // the world), tracking the sun's lateral sweep for a little motion.
  const Vec3 moon_dir = Normalize(Vec3{ca * 0.3f, -1.0f, -0.25f});
  const Vec3 moon_color = {0.45f, 0.55f, 0.85f};
  constexpr f32 kMoonIntensity = 0.25f;

  SkyLighting out;
  out.sun_direction = Normalize(Lerp(moon_dir, day_dir, day));
  out.sun_intensity = LerpF(kMoonIntensity, kDayIntensity, day);
  out.sun_color = Lerp(moon_color, day_color, day);
  out.ambient = LerpF(0.02f, 0.08f, day);
  return out;
}

}  // namespace rec
