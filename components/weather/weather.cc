#include "components/weather/weather.h"

#include <cmath>

namespace rx::weather {
namespace {

u64 Mix(u64 x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return x;
}
f32 Clamp01(f32 x) {
  return x < 0 ? 0 : (x > 1 ? 1 : x);
}
f32 Smooth(f32 t) {
  t = Clamp01(t);
  return t * t * (3.0f - 2.0f * t);
}
f32 LerpF(f32 a, f32 b, f32 t) {
  return a + (b - a) * t;
}

// Yaw interpolation goes the short way around the circle, so a transition from
// 350 degrees to 10 degrees swings 20 degrees through north, not 340 back.
f32 LerpYaw(f32 a, f32 b, f32 t) {
  constexpr f32 kTau = 6.28318530718f;
  f32 delta = std::fmod(b - a, kTau);
  if (delta > kTau * 0.5f)
    delta -= kTau;
  if (delta < -kTau * 0.5f)
    delta += kTau;
  f32 yaw = std::fmod(a + delta * t, kTau);
  return yaw < 0 ? yaw + kTau : yaw;
}

const WeatherDef kDefault;  // pleasant, returned when the climate is empty

}  // namespace

void WeatherDef::DeriveFromKind() {
  switch (kind) {
    case Kind::kPleasant:
      cloud_coverage = 0.18f;
      cloud_density = 1.0f;
      // Fair-weather cumulus: bases on the condensation level, modest tops.
      cloud_type = 0.85f;
      storminess = 0.0f;
      darkness = 0.0f;
      base_altitude = 1500.0f;
      top_altitude = 4000.0f;
      turbulence = 0.7f;
      vertical_skew = 500.0f;
      fog_height = 70.0f;
      fog_churn = 0.10f;
      aerosol = 0.0f;
      light_scale = 1.0f;
      light_tint = {1, 1, 1};
      precipitation = 0.0f;
      snow = false;
      gustiness = 0.10f;
      thunder_period = -1.0f;
      aurora = 1.0f;  // clear Skyrim nights show the full curtains (game-gated)
      break;
    case Kind::kCloudy:
      cloud_coverage = 0.55f;
      cloud_density = 1.0f;
      // Stratocumulus deck: lower, flatter and broader than fair-weather cumulus.
      cloud_type = 0.45f;
      storminess = 0.05f;
      darkness = 0.05f;
      base_altitude = 1000.0f;
      top_altitude = 2600.0f;
      turbulence = 0.9f;
      vertical_skew = 600.0f;
      fog_height = 90.0f;
      fog_churn = 0.15f;
      aerosol = 0.15f;
      light_scale = 0.72f;
      light_tint = {0.95f, 0.96f, 1.0f};
      precipitation = 0.0f;
      snow = false;
      gustiness = 0.25f;
      thunder_period = -1.0f;
      aurora = 0.35f;  // a faint shimmer through the gaps
      break;
    case Kind::kRainy:
      cloud_coverage = 0.92f;
      cloud_density = 1.4f;
      // Cumulonimbus: a low base under a towering anvil, dark and churning.
      cloud_type = 1.0f;
      storminess = 0.85f;
      darkness = 0.55f;
      base_altitude = 700.0f;
      top_altitude = 11000.0f;
      turbulence = 1.5f;
      vertical_skew = 1400.0f;
      fog_height = 120.0f;
      fog_churn = 0.45f;
      aerosol = 0.40f;
      light_scale = 0.42f;
      light_tint = {0.80f, 0.85f, 0.95f};
      precipitation = 0.85f;
      snow = false;
      thunder = true;
      gustiness = 0.70f;  // storms squall
      thunder_period = 12.0f;
      aurora = 0.0f;
      break;
    case Kind::kSnow:
      cloud_coverage = 0.85f;
      cloud_density = 1.2f;
      // Nimbostratus: a thick, featureless ceiling sitting low over the ground.
      cloud_type = 0.2f;
      storminess = 0.15f;
      darkness = 0.3f;
      base_altitude = 600.0f;
      top_altitude = 3500.0f;
      turbulence = 1.1f;
      vertical_skew = 900.0f;
      fog_height = 140.0f;
      fog_churn = 0.25f;
      aerosol = 0.45f;
      light_scale = 0.60f;
      light_tint = {0.92f, 0.95f, 1.0f};
      precipitation = 0.80f;
      snow = true;
      gustiness = 0.55f;  // driving snow; a real blizzard raises this via wind
      thunder_period = -1.0f;
      aurora = 0.0f;
      break;
  }
}

WeatherState ToState(const WeatherDef& d) {
  WeatherState s;
  s.cloud_coverage = d.cloud_coverage;
  s.cloud_density = d.cloud_density;
  s.cloud_type = d.cloud_type;
  s.storminess = d.storminess;
  s.darkness = d.darkness;
  s.base_altitude = d.base_altitude;
  s.top_altitude = d.top_altitude;
  s.turbulence = d.turbulence;
  s.vertical_skew = d.vertical_skew;
  s.fog_height = d.fog_height;
  s.fog_churn = d.fog_churn;
  s.aerosol = d.aerosol;
  s.wind = d.wind;
  s.light_scale = d.light_scale;
  s.light_tint = d.light_tint;
  s.precipitation = d.precipitation;
  s.snow = d.snow;
  s.thunder = d.thunder;
  s.wind_yaw = d.wind_yaw;
  s.gustiness = d.gustiness;
  s.lightning_color = d.lightning_color;
  s.thunder_period = d.thunder_period;
  s.aurora = d.aurora;
  return s;
}

WeatherState Lerp(const WeatherState& a, const WeatherState& b, f32 t) {
  WeatherState s;
  s.cloud_coverage = LerpF(a.cloud_coverage, b.cloud_coverage, t);
  s.cloud_density = LerpF(a.cloud_density, b.cloud_density, t);
  s.cloud_type = LerpF(a.cloud_type, b.cloud_type, t);
  s.storminess = LerpF(a.storminess, b.storminess, t);
  s.darkness = LerpF(a.darkness, b.darkness, t);
  s.base_altitude = LerpF(a.base_altitude, b.base_altitude, t);
  s.top_altitude = LerpF(a.top_altitude, b.top_altitude, t);
  s.turbulence = LerpF(a.turbulence, b.turbulence, t);
  s.vertical_skew = LerpF(a.vertical_skew, b.vertical_skew, t);
  s.fog_height = LerpF(a.fog_height, b.fog_height, t);
  s.fog_churn = LerpF(a.fog_churn, b.fog_churn, t);
  s.aerosol = LerpF(a.aerosol, b.aerosol, t);
  s.wind = LerpF(a.wind, b.wind, t);
  s.light_scale = LerpF(a.light_scale, b.light_scale, t);
  s.light_tint = {LerpF(a.light_tint.x, b.light_tint.x, t),
                  LerpF(a.light_tint.y, b.light_tint.y, t),
                  LerpF(a.light_tint.z, b.light_tint.z, t)};
  s.precipitation = LerpF(a.precipitation, b.precipitation, t);
  s.snow = t < 0.5f ? a.snow : b.snow;
  s.thunder = t < 0.5f ? a.thunder : b.thunder;
  s.wind_yaw = LerpYaw(a.wind_yaw, b.wind_yaw, t);
  s.gustiness = LerpF(a.gustiness, b.gustiness, t);
  s.lightning_color = {LerpF(a.lightning_color.x, b.lightning_color.x, t),
                       LerpF(a.lightning_color.y, b.lightning_color.y, t),
                       LerpF(a.lightning_color.z, b.lightning_color.z, t)};
  // The strike cadence follows the dominant side whole: blending toward a
  // non-thundery weather (period < 0) must not produce nonsense periods.
  s.thunder_period = t < 0.5f ? a.thunder_period : b.thunder_period;
  s.aurora = LerpF(a.aurora, b.aurora, t);
  return s;
}

void WeatherSystem::SetClimate(base::Vector<base::Pair<WeatherDef, u32>> weighted) {
  climate_ = base::move(weighted);
  total_chance_ = 0;
  for (auto& [def, chance] : climate_)
    total_chance_ += (chance == 0 ? 1 : chance);
}

const WeatherDef& WeatherSystem::ForSlot(i64 slot) const {
  if (climate_.empty())
    return kDefault;
  u64 h = Mix(seed_ ^ (static_cast<u64>(slot) * 0x9e3779b97f4a7c15ULL));
  u32 pick = static_cast<u32>(h % (total_chance_ == 0 ? 1 : total_chance_));
  u32 acc = 0;
  for (auto& [def, chance] : climate_) {
    acc += (chance == 0 ? 1 : chance);
    if (pick < acc)
      return def;
  }
  return climate_.back().first;
}

namespace {
i64 SlotOf(f64 game_days, f32 hold_hours) {
  double slot_days = hold_hours / 24.0;
  return static_cast<i64>(std::floor(game_days / slot_days));
}
}  // namespace

f32 WeatherSystem::Transition(f64 game_days) const {
  if (climate_.empty())
    return 0;
  double slot_days = hold_hours_ / 24.0;
  i64 slot = SlotOf(game_days, hold_hours_);
  double phase = game_days / slot_days - static_cast<double>(slot);
  double tr = transition_hours_ / hold_hours_;
  if (tr <= 0 || phase <= 1.0 - tr)
    return 0;
  return Smooth(static_cast<f32>((phase - (1.0 - tr)) / tr));
}

const WeatherDef& WeatherSystem::Current(f64 game_days) const {
  return ForSlot(SlotOf(game_days, hold_hours_));
}
const WeatherDef& WeatherSystem::Target(f64 game_days) const {
  return ForSlot(SlotOf(game_days, hold_hours_) + 1);
}

WeatherState WeatherSystem::At(f64 game_days) const {
  if (climate_.empty())
    return {};
  f32 t = Transition(game_days);
  if (t <= 0)
    return ToState(Current(game_days));
  return Lerp(ToState(Current(game_days)), ToState(Target(game_days)), t);
}

namespace {
// Ray-casting point-in-polygon test.
bool PointInPoly(const base::Vector<base::Pair<f32, f32>>& poly, f32 x, f32 y) {
  bool inside = false;
  const mem_size n = poly.size();
  if (n < 3)
    return false;
  for (mem_size i = 0, j = n - 1; i < n; j = i++) {
    f32 xi = poly[i].first, yi = poly[i].second;
    f32 xj = poly[j].first, yj = poly[j].second;
    if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi))
      inside = !inside;
  }
  return inside;
}
}  // namespace

const base::Vector<base::Pair<WeatherDef, u32>>* RegionWeather::ClimateAt(f32 x,
                                                                          f32 y,
                                                                          u64* out_region) const {
  const Region* best = nullptr;
  for (const Region& r : regions_) {
    if (r.climate.empty())
      continue;
    if (!PointInPoly(r.polygon, x, y))
      continue;
    if (!best || r.priority > best->priority)
      best = &r;
  }
  if (out_region)
    *out_region = best ? best->form : 0;
  return best ? &best->climate : nullptr;
}

}  // namespace rx::weather
