#include "script/games/skyrim/skyrim_natives_ext.h"

#include <cmath>
#include <numbers>

namespace rec::script::skyrim {

using papyrus::ObjectRef;
using papyrus::Value;
using papyrus::VirtualMachine;

namespace {

// Models the sun as a point on a half-circle: an hour angle of 0 at noon puts it
// straight up, +/- pi at midnight straight down, the horizon crossings fall at
// 06:00 and 18:00. The track is given a small southern tilt so the path is not
// exactly overhead, which yields a real Y component; the result stays unit length.
constexpr f32 kSunTrackTilt = 0.15f;  // radians the daytime arc leans off east to west

struct SunDir {
  f32 x;
  f32 y;
  f32 z;
};

SunDir ComputeSunDirection(f32 game_time_days) {
  const f32 time_of_day = game_time_days - std::floor(game_time_days);
  const f32 hour_angle = (time_of_day - 0.5f) * 2.0f * std::numbers::pi_v<f32>;
  const f32 elevation = std::cos(hour_angle);  // +1 up at noon, -1 down at midnight
  const f32 horizontal = std::sin(hour_angle);  // -1 at sunrise, +1 at sunset
  return SunDir{horizontal * std::cos(kSunTrackTilt), horizontal * std::sin(kSunTrackTilt), elevation};
}

}  // namespace

void RegisterGameEnvironment(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  reg.Register("Game", "GetSunPositionX", [bindings](VirtualMachine&, ObjectRef, ext::Args&) {
    return Value::Float(ComputeSunDirection(ext::Resolve(bindings).GetCurrentGameTime()).x);
  });
  reg.Register("Game", "GetSunPositionY", [bindings](VirtualMachine&, ObjectRef, ext::Args&) {
    return Value::Float(ComputeSunDirection(ext::Resolve(bindings).GetCurrentGameTime()).y);
  });
  reg.Register("Game", "GetSunPositionZ", [bindings](VirtualMachine&, ObjectRef, ext::Args&) {
    return Value::Float(ComputeSunDirection(ext::Resolve(bindings).GetCurrentGameTime()).z);
  });

  reg.Register("Game", "GetGameSettingInt", [bindings](VirtualMachine&, ObjectRef, ext::Args& a) {
    return Value::Int(static_cast<i32>(ext::Resolve(bindings).GetGameSettingFloat(ext::ArgS(a, 0))));
  });
}

}  // namespace rec::script::skyrim
