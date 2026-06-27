#include "weather/weather.h"

#include <cmath>

namespace rec::weather {
namespace {

u64 Mix(u64 x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return x;
}
f32 Clamp01(f32 x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
f32 Smooth(f32 t) {
  t = Clamp01(t);
  return t * t * (3.0f - 2.0f * t);
}
f32 LerpF(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

const WeatherDef kDefault;  // pleasant, returned when the climate is empty

}  // namespace

void WeatherDef::DeriveFromKind() {
  switch (kind) {
    case Kind::kPleasant:
      cloud_coverage = 0.18f;
      cloud_density = 1.0f;
      aerosol = 0.0f;
      light_scale = 1.0f;
      light_tint = {1, 1, 1};
      precipitation = 0.0f;
      snow = false;
      break;
    case Kind::kCloudy:
      cloud_coverage = 0.55f;
      cloud_density = 1.0f;
      aerosol = 0.15f;
      light_scale = 0.72f;
      light_tint = {0.95f, 0.96f, 1.0f};
      precipitation = 0.0f;
      snow = false;
      break;
    case Kind::kRainy:
      cloud_coverage = 0.92f;
      cloud_density = 1.4f;
      aerosol = 0.40f;
      light_scale = 0.42f;
      light_tint = {0.80f, 0.85f, 0.95f};
      precipitation = 0.85f;
      snow = false;
      break;
    case Kind::kSnow:
      cloud_coverage = 0.85f;
      cloud_density = 1.2f;
      aerosol = 0.45f;
      light_scale = 0.60f;
      light_tint = {0.92f, 0.95f, 1.0f};
      precipitation = 0.80f;
      snow = true;
      break;
  }
}

WeatherState ToState(const WeatherDef& d) {
  WeatherState s;
  s.cloud_coverage = d.cloud_coverage;
  s.cloud_density = d.cloud_density;
  s.aerosol = d.aerosol;
  s.wind = d.wind;
  s.light_scale = d.light_scale;
  s.light_tint = d.light_tint;
  s.precipitation = d.precipitation;
  s.snow = d.snow;
  return s;
}

WeatherState Lerp(const WeatherState& a, const WeatherState& b, f32 t) {
  WeatherState s;
  s.cloud_coverage = LerpF(a.cloud_coverage, b.cloud_coverage, t);
  s.cloud_density = LerpF(a.cloud_density, b.cloud_density, t);
  s.aerosol = LerpF(a.aerosol, b.aerosol, t);
  s.wind = LerpF(a.wind, b.wind, t);
  s.light_scale = LerpF(a.light_scale, b.light_scale, t);
  s.light_tint = {LerpF(a.light_tint.x, b.light_tint.x, t),
                  LerpF(a.light_tint.y, b.light_tint.y, t),
                  LerpF(a.light_tint.z, b.light_tint.z, t)};
  s.precipitation = LerpF(a.precipitation, b.precipitation, t);
  s.snow = t < 0.5f ? a.snow : b.snow;
  return s;
}

void WeatherSystem::SetClimate(std::vector<std::pair<WeatherDef, u32>> weighted) {
  climate_ = std::move(weighted);
  total_chance_ = 0;
  for (auto& [def, chance] : climate_) total_chance_ += (chance == 0 ? 1 : chance);
}

const WeatherDef& WeatherSystem::ForSlot(i64 slot) const {
  if (climate_.empty()) return kDefault;
  u64 h = Mix(seed_ ^ (static_cast<u64>(slot) * 0x9e3779b97f4a7c15ULL));
  u32 pick = static_cast<u32>(h % (total_chance_ == 0 ? 1 : total_chance_));
  u32 acc = 0;
  for (auto& [def, chance] : climate_) {
    acc += (chance == 0 ? 1 : chance);
    if (pick < acc) return def;
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
  if (climate_.empty()) return 0;
  double slot_days = hold_hours_ / 24.0;
  i64 slot = SlotOf(game_days, hold_hours_);
  double phase = game_days / slot_days - static_cast<double>(slot);
  double tr = transition_hours_ / hold_hours_;
  if (tr <= 0 || phase <= 1.0 - tr) return 0;
  return Smooth(static_cast<f32>((phase - (1.0 - tr)) / tr));
}

const WeatherDef& WeatherSystem::Current(f64 game_days) const {
  return ForSlot(SlotOf(game_days, hold_hours_));
}
const WeatherDef& WeatherSystem::Target(f64 game_days) const {
  return ForSlot(SlotOf(game_days, hold_hours_) + 1);
}

WeatherState WeatherSystem::At(f64 game_days) const {
  if (climate_.empty()) return {};
  f32 t = Transition(game_days);
  if (t <= 0) return ToState(Current(game_days));
  return Lerp(ToState(Current(game_days)), ToState(Target(game_days)), t);
}

namespace {
// Ray-casting point-in-polygon test.
bool PointInPoly(const std::vector<std::pair<f32, f32>>& poly, f32 x, f32 y) {
  bool inside = false;
  const size_t n = poly.size();
  if (n < 3) return false;
  for (size_t i = 0, j = n - 1; i < n; j = i++) {
    f32 xi = poly[i].first, yi = poly[i].second;
    f32 xj = poly[j].first, yj = poly[j].second;
    if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi)) inside = !inside;
  }
  return inside;
}
}  // namespace

const std::vector<std::pair<WeatherDef, u32>>* RegionWeather::ClimateAt(f32 x, f32 y,
                                                                        u64* out_region) const {
  const Region* best = nullptr;
  for (const Region& r : regions_) {
    if (r.climate.empty()) continue;
    if (!PointInPoly(r.polygon, x, y)) continue;
    if (!best || r.priority > best->priority) best = &r;
  }
  if (out_region) *out_region = best ? best->form : 0;
  return best ? &best->climate : nullptr;
}

}  // namespace rec::weather
