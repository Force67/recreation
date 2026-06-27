// weathertest: the deterministic weather selection + cross-fade logic that maps
// a Bethesda-style climate (weighted weather list) onto our physical pipeline.
// No game data, so it runs in the default ctest gate.
#include <algorithm>
#include <cstdio>

#include "weather/weather.h"

using rec::weather::WeatherDef;
using rec::weather::WeatherState;
using rec::weather::WeatherSystem;

static WeatherDef Make(WeatherDef::Kind k) {
  WeatherDef d;
  d.kind = k;
  d.DeriveFromKind();
  return d;
}

int main() {
  int failures = 0;
  auto check = [&](const char* what, bool ok) {
    std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
  };

  // A single-weather climate is constant.
  WeatherSystem clear;
  clear.SetClimate({{Make(WeatherDef::Kind::kPleasant), 1}});
  clear.set_seed(1234);
  bool constant = true;
  for (int i = 0; i < 200; ++i)
    if (clear.At(i * 0.31).cloud_coverage > 0.3f) constant = false;
  check("single-weather climate is constant", constant);

  // The weighted distribution roughly matches the chances over many slots.
  WeatherSystem clim;
  clim.SetClimate({{Make(WeatherDef::Kind::kPleasant), 70},
                   {Make(WeatherDef::Kind::kCloudy), 20},
                   {Make(WeatherDef::Kind::kRainy), 10}});
  clim.set_seed(99);
  clim.set_timing(5.0f, 0.6f);
  int counts[4] = {};
  const int kSlots = 8000;
  for (int s = 0; s < kSlots; ++s) {
    double game_days = (s + 0.3) * (5.0 / 24.0);  // mid-slot (no transition)
    counts[static_cast<int>(clim.Current(game_days).kind)]++;
  }
  double pleasant = 100.0 * counts[0] / kSlots;
  double cloudy = 100.0 * counts[1] / kSlots;
  double rainy = 100.0 * counts[2] / kSlots;
  std::printf("    distribution: pleasant %.1f%%  cloudy %.1f%%  rainy %.1f%%\n", pleasant, cloudy,
              rainy);
  check("pleasant ~70%", pleasant > 64 && pleasant < 76);
  check("cloudy ~20%", cloudy > 15 && cloudy < 25);
  check("rainy ~10%", rainy > 6 && rainy < 14);

  // Deterministic: same seed + game time -> same state.
  check("deterministic", clim.At(123.456).cloud_coverage == clim.At(123.456).cloud_coverage);

  // Weathers cross-fade at the slot boundary: deep in a transition between two
  // different weathers, the blended coverage lies strictly between them.
  bool blended = false;
  const double tr = 0.6 / 5.0;  // transition fraction of a slot
  for (int s = 0; s < 400 && !blended; ++s) {
    double slot_days = 5.0 / 24.0;
    const WeatherDef& a = clim.Current((s + 0.5) * slot_days);
    const WeatherDef& b = clim.Current((s + 1.5) * slot_days);
    if (a.kind == b.kind) continue;
    double t_days = (s + (1.0 - tr * 0.5)) * slot_days;  // middle of the cross-fade
    WeatherState mid = clim.At(t_days);
    float lo = std::min(a.cloud_coverage, b.cloud_coverage);
    float hi = std::max(a.cloud_coverage, b.cloud_coverage);
    if (clim.Transition(t_days) > 0 && mid.cloud_coverage > lo + 0.001f &&
        mid.cloud_coverage < hi - 0.001f)
      blended = true;
  }
  check("weathers cross-fade at the boundary", blended);

  // An empty climate yields the neutral default (demo scenes etc.).
  WeatherSystem none;
  check("empty climate is neutral default", none.empty() && none.At(5.0).cloud_coverage < 0.3f);

  // Region weather: point-in-polygon climate selection, priority on overlap.
  rec::weather::RegionWeather rw;
  rec::weather::Region r1;
  r1.form = 1;
  r1.priority = 0;
  r1.polygon = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};  // square [0,10]^2
  r1.climate = {{Make(WeatherDef::Kind::kRainy), 1}};
  rw.Add(r1);
  rec::weather::Region r2;
  r2.form = 2;
  r2.priority = 5;  // higher priority, overlaps r1's upper-right
  r2.polygon = {{5, 5}, {15, 5}, {15, 15}, {5, 15}};
  r2.climate = {{Make(WeatherDef::Kind::kSnow), 1}};
  rw.Add(r2);
  rec::u64 reg = 0;
  check("point inside region 1", rw.ClimateAt(2.0f, 2.0f, &reg) != nullptr && reg == 1);
  check("point outside all regions", rw.ClimateAt(20.0f, 20.0f, &reg) == nullptr && reg == 0);
  check("overlap picks higher priority", rw.ClimateAt(7.0f, 7.0f, &reg) != nullptr && reg == 2);

  std::printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}
