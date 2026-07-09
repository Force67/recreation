#include "script/games/skyrim/skyrim_natives_ext.h"

#include <cmath>
#include <string>

namespace rx::script::skyrim {

using papyrus::ObjectRef;
using papyrus::Value;
using papyrus::VirtualMachine;

void RegisterUtilityExtra(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  // Formats a game time in days (the value GetCurrentGameTime returns) as a day
  // number and a 24-hour clock, e.g. "Day 12, 14:30".
  reg.Register("Utility", "GameTimeToString", [](VirtualMachine&, ObjectRef, ext::Args& a) {
    f32 game_time = ext::ArgF(a, 0);
    f64 days = std::floor(static_cast<f64>(game_time));
    f64 fraction_of_day = static_cast<f64>(game_time) - days;
    i32 total_minutes = static_cast<i32>(std::lround(fraction_of_day * 1440.0));
    if (total_minutes >= 1440) {  // rounding pushed past midnight, roll to next day
      total_minutes -= 1440;
      days += 1.0;
    }
    i32 hours = total_minutes / 60;
    i32 minutes = total_minutes % 60;
    auto pad = [](i32 v) {
      std::string s = std::to_string(v);
      return s.size() < 2 ? "0" + s : s;
    };
    std::string out = "Day " + std::to_string(static_cast<i64>(days)) + ", " + pad(hours) +
                      ":" + pad(minutes);
    return Value::Str(out);
  });

  // Steady default frame rates until frame-time capture is wired.
  auto frame_rate = [](VirtualMachine&, ObjectRef, ext::Args&) { return Value::Float(60.0f); };
  reg.Register("Utility", "GetMaxFrameRate", frame_rate);
  reg.Register("Utility", "GetMinFrameRate", frame_rate);
  reg.Register("Utility", "GetAverageFrameRate", frame_rate);
}

}  // namespace rx::script::skyrim
