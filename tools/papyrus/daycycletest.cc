// daycycletest: the world clock and its day/night sun model, plus the time
// Papyrus surface (Utility.GetCurrentGameTime and the GameHour/GameDaysPassed/
// TimeScale globals) reading and writing the clock through the bindings. Needs
// no game data, so it runs in the default ctest gate.
#include <cmath>
#include <cstdio>

#include "core/world_clock.h"
#include "script/games/skyrim/skyrim_bindings.h"

using rx::ComputeSkyLighting;
using rx::SkyLighting;
using rx::WorldClock;
using rx::script::papyrus::ObjectRef;
using rx::script::skyrim::RecordBackedSkyrimBindings;

int main() {
  int failures = 0;
  auto check = [&](const char* what, bool ok) {
    std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
  };
  auto approx = [](rx::f32 a, rx::f32 b) { return std::fabs(a - b) < 1e-3f; };

  // --- Clock: advance, wrap, and the days/timescale relationship. ---
  WorldClock clock;
  clock.Configure(12.0f, 20.0f);  // noon, Bethesda's default timescale
  check("starts at hour 12", approx(clock.hour(), 12.0f));
  clock.Advance(3600.0);  // one real hour -> 20 game hours
  check("after 1 real hour, +20h wraps to 08:00", approx(clock.hour(), 8.0f));
  check("game_days advanced by 20/24", std::fabs(clock.game_days() - (32.0 / 24.0)) < 1e-6);
  check("real_hours tracks wall clock", approx(clock.real_hours(), 1.0f));
  clock.set_hour(0.0f);
  check("set_hour 0 -> midnight, keeps day count", approx(clock.hour(), 0.0f) &&
                                                       std::floor(clock.game_days()) == 1.0);
  clock.set_timescale(0.0f);
  clock.Advance(3600.0);
  check("timescale 0 freezes game time", approx(clock.hour(), 0.0f));

  // --- Sun model: bright overhead by day, dim from above at night, east->west. ---
  const SkyLighting noon = ComputeSkyLighting(12.0f);
  check("noon sun points down (overhead)", noon.sun_direction.y < -0.7f);
  check("noon is bright", noon.sun_intensity > 2.0f);
  const SkyLighting midnight = ComputeSkyLighting(0.0f);
  check("midnight is dim", midnight.sun_intensity < 0.5f);
  check("midnight lights from above", midnight.sun_direction.y < 0.0f);
  check("night ambient below day ambient", midnight.ambient < noon.ambient);
  const SkyLighting sunrise = ComputeSkyLighting(6.5f);
  check("sunrise light travels west (-x)", sunrise.sun_direction.x < 0.0f);
  const SkyLighting sunset = ComputeSkyLighting(17.5f);
  check("sunset light travels east (+x)", sunset.sun_direction.x > 0.0f);

  // --- Natives through the bindings. ---
  RecordBackedSkyrimBindings bindings;  // no records needed for the clock surface
  WorldClock clock2;
  clock2.Configure(9.0f, 20.0f);
  bindings.set_clock(&clock2);
  const rx::u64 hour_glob = 0x38, days_glob = 0x39, ts_glob = 0x3A;  // Skyrim time GLOBs
  bindings.set_time_globals(hour_glob, days_glob, ts_glob);
  check("GetCurrentGameTime mirrors game_days",
        approx(bindings.GetCurrentGameTime(), static_cast<rx::f32>(clock2.game_days())));
  check("GameHour global reads clock", approx(bindings.GetGlobalValue(ObjectRef{hour_glob}), 9.0f));
  check("GameDaysPassed global reads clock",
        approx(bindings.GetGlobalValue(ObjectRef{days_glob}), static_cast<rx::f32>(9.0 / 24.0)));
  check("TimeScale global reads clock", approx(bindings.GetGlobalValue(ObjectRef{ts_glob}), 20.0f));
  // Writing a time global moves the clock.
  bindings.SetGlobalValue(ObjectRef{hour_glob}, 18.0f);
  check("SetValue(GameHour, 18) -> clock hour 18", approx(clock2.hour(), 18.0f));
  bindings.SetGlobalValue(ObjectRef{ts_glob}, 100.0f);
  check("SetValue(TimeScale, 100) -> clock timescale 100", approx(clock2.timescale(), 100.0f));
  // An unrelated global still uses the stored/authored value, untouched by the clock.
  bindings.SetGlobalValue(ObjectRef{0x999}, 5.0f);
  check("unrelated global unaffected", approx(bindings.GetGlobalValue(ObjectRef{0x999}), 5.0f));
  // Without a clock the time natives fall back to neutral defaults.
  RecordBackedSkyrimBindings bare;
  check("no clock -> GetCurrentGameTime 0", bare.GetCurrentGameTime() == 0.0f);

  std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}
