// weathertest: the deterministic weather selection + cross-fade logic that maps
// a Bethesda-style climate (weighted weather list) onto our physical pipeline.
// No game data, so it runs in the default ctest gate.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "weather/director.h"
#include "weather/weather.h"

using rx::weather::Director;
using rx::weather::Tick;
using rx::weather::WeatherDef;
using rx::weather::WeatherState;
using rx::weather::WeatherSystem;

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
  rx::weather::RegionWeather rw;
  rx::weather::Region r1;
  r1.form = 1;
  r1.priority = 0;
  r1.polygon = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};  // square [0,10]^2
  r1.climate = {{Make(WeatherDef::Kind::kRainy), 1}};
  rw.Add(r1);
  rx::weather::Region r2;
  r2.form = 2;
  r2.priority = 5;  // higher priority, overlaps r1's upper-right
  r2.polygon = {{5, 5}, {15, 5}, {15, 15}, {5, 15}};
  r2.climate = {{Make(WeatherDef::Kind::kSnow), 1}};
  rw.Add(r2);
  rx::u64 reg = 0;
  check("point inside region 1", rw.ClimateAt(2.0f, 2.0f, &reg) != nullptr && reg == 1);
  check("point outside all regions", rw.ClimateAt(20.0f, 20.0f, &reg) == nullptr && reg == 0);
  check("overlap picks higher priority", rw.ClimateAt(7.0f, 7.0f, &reg) != nullptr && reg == 2);

  // Yaw interpolation goes the short way around: 6.1 rad -> 0.1 rad swings
  // through 0/tau (a ~0.28 rad move), never back through pi.
  {
    WeatherState ya, yb;
    ya.wind_yaw = 6.1f;
    yb.wind_yaw = 0.1f;
    const float mid = rx::weather::Lerp(ya, yb, 0.5f).wind_yaw;
    const float end = rx::weather::Lerp(ya, yb, 1.0f).wind_yaw;
    check("yaw lerp wraps the short way", std::abs(mid - 6.2416f) < 0.01f);
    check("yaw lerp lands on the target", std::abs(end - 0.1f) < 0.01f);
    check("yaw lerp stays off the long path", std::abs(mid - 3.1f) > 1.0f);
  }

  // The thunder cadence follows the dominant side whole, so a fade toward a
  // non-thundery weather never yields a bogus in-between period.
  {
    WeatherState ta, tb;
    ta.thunder_period = 8.0f;
    tb.thunder_period = -1.0f;
    check("thunder period follows the dominant side",
          rx::weather::Lerp(ta, tb, 0.3f).thunder_period == 8.0f &&
              rx::weather::Lerp(ta, tb, 0.7f).thunder_period == -1.0f);
  }

  // Director strike scheduling is deterministic: two directors sharing content,
  // seed and clock agree on every strike (seed, position, flash) -- the
  // multiplayer contract. No audio bound, so the directors are pure logic.
  {
    WeatherDef storm = Make(WeatherDef::Kind::kRainy);
    storm.thunder = true;
    storm.thunder_period = 8.0f;
    Director a, b;
    a.SetContent({{storm, 1}}, {}, 42);
    b.SetContent({{storm, 1}}, {}, 42);
    rx::render::RenderSettings sa, sb;
    bool agree = true;
    int strikes = 0;
    rx::u32 last_seed = 0;
    for (int i = 0; i < 1200; ++i) {
      Tick t;
      t.real_seconds = static_cast<float>(i) * 0.1f;
      t.game_days = static_cast<double>(t.real_seconds) / 86400.0;
      t.frame_delta = 0.1f;
      t.timescale = 1.0f;
      t.anchor = {100.0f, 5.0f, -50.0f};
      t.listener = t.anchor;
      a.Update(t, &sa.weather, &sa);
      b.Update(t, &sb.weather, &sb);
      if (sa.weather.strike_seed != sb.weather.strike_seed ||
          sa.weather.lightning != sb.weather.lightning ||
          sa.weather.strike_pos.x != sb.weather.strike_pos.x)
        agree = false;
      if (sa.weather.strike_seed != last_seed) {
        last_seed = sa.weather.strike_seed;
        ++strikes;
      }
    }
    check("two directors agree on every strike", agree);
    std::printf("    strikes over 120 s at 8 s period: %d\n", strikes);
    check("strike cadence tracks the thunder period", strikes >= 10 && strikes <= 17);

    // A director joining mid-storm converges onto the same schedule: after one
    // full period it reports the same strike as one that ran from the start.
    Director late;
    late.SetContent({{storm, 1}}, {}, 42);
    rx::render::RenderSettings sl;
    for (int i = 900; i < 1200; ++i) {
      Tick t;
      t.real_seconds = static_cast<float>(i) * 0.1f;
      t.game_days = static_cast<double>(t.real_seconds) / 86400.0;
      t.frame_delta = 0.1f;
      t.timescale = 1.0f;
      t.anchor = {100.0f, 5.0f, -50.0f};
      t.listener = t.anchor;
      late.Update(t, &sl.weather, &sl);
    }
    check("a late joiner lands on the shared schedule",
          sl.weather.strike_seed == sa.weather.strike_seed);
  }

  // Integrators: rain soaks the ground monotonically, clearing dries it; snow
  // cover starts at exactly zero and only builds under snowfall.
  {
    Director d;
    d.SetContent({{Make(WeatherDef::Kind::kRainy), 1}}, {}, 7);
    rx::render::RenderSettings s;
    float base = 0.0f;
    auto run = [&](float seconds, float dt) {
      for (float t = 0.0f; t < seconds; t += dt) {
        Tick tick;
        tick.real_seconds = base + t;
        tick.game_days = 0.001;  // frozen clock: one settled rainy weather
        tick.frame_delta = dt;
        tick.timescale = 1.0f;
        d.Update(tick, &s.weather, &s);
      }
      base += seconds;
    };
    run(1.0f, 0.05f);
    const float early = s.weather.wetness;
    run(59.0f, 0.05f);
    const float soaked = s.weather.wetness;
    check("rain wets the ground monotonically", soaked > early && early > 0.0f);
    check("a minute of heavy rain nearly soaks it", soaked > 0.8f);
    check("rain leaves the snow cover untouched", s.weather.snow_cover == 0.0f);
    // Clearing up: override to pleasant and let it dry.
    WeatherState clear_state = rx::weather::ToState(Make(WeatherDef::Kind::kPleasant));
    d.SetOverride(&clear_state);
    run(120.0f, 0.05f);
    check("clear weather dries the ground", s.weather.wetness < soaked * 0.35f);
  }

  // Aurora: pleasant nights carry it, the director gates it per game.
  {
    Director d;
    d.SetContent({{Make(WeatherDef::Kind::kPleasant), 1}}, {}, 3);
    rx::render::RenderSettings s;
    Tick t;
    t.frame_delta = 0.016f;
    t.timescale = 1.0f;
    d.Update(t, &s.weather, &s);
    check("aurora stays gated off by default", !s.weather.aurora);
    d.set_aurora_allowed(true);
    d.Update(t, &s.weather, &s);
    check("pleasant weather shows the aurora when allowed",
          s.weather.aurora && s.weather.aurora_intensity > 0.9f);
  }

  std::printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}
