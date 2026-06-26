#ifndef RECREATION_WEATHER_WEATHER_H_
#define RECREATION_WEATHER_WEATHER_H_

#include <string>
#include <utility>
#include <vector>

#include "core/math.h"
#include "core/types.h"

namespace rec::weather {

// Renderer-agnostic weather parameters. Parsed from a Bethesda WTHR record (or
// built by hand for tests) and derived into physical knobs our sky / cloud /
// atmosphere pipeline consumes -- never the game's baked sky textures, sprite
// clouds or flat distance fog.
struct WeatherDef {
  u64 form = 0;
  std::string editor_id;
  enum class Kind : u8 { kPleasant, kCloudy, kRainy, kSnow };
  Kind kind = Kind::kPleasant;

  f32 cloud_coverage = 0.18f;  // volumetric cloud coverage 0..1
  f32 cloud_density = 1.0f;
  f32 aerosol = 0.0f;        // haze / turbidity 0..1, scales aerial perspective
  f32 wind = 10.0f;          // cloud drift, m/s
  f32 light_scale = 1.0f;    // multiplies sun intensity (overcast dims the light)
  Vec3 light_tint{1, 1, 1};  // multiplies sun colour (overcast greys it)
  f32 precipitation = 0.0f;  // 0 none .. 1 heavy
  bool snow = false;         // precipitation is snow vs rain
  bool thunder = false;      // lightning strikes (heavy rain, FO4 radstorms)

  // Fills the physical knobs from the classification. A record loader can refine
  // these afterwards from the weather's authored fog / colour data.
  void DeriveFromKind();
};

// The blended, per-frame weather the engine pushes into the renderer.
struct WeatherState {
  f32 cloud_coverage = 0.18f;
  f32 cloud_density = 1.0f;
  f32 aerosol = 0.0f;
  f32 wind = 10.0f;
  f32 light_scale = 1.0f;
  Vec3 light_tint{1, 1, 1};
  f32 precipitation = 0.0f;
  bool snow = false;
  bool thunder = false;
};

WeatherState ToState(const WeatherDef& def);
WeatherState Lerp(const WeatherState& a, const WeatherState& b, f32 t);

// A Bethesda REGN region's weather: a worldspace area (a polygon in game units)
// with its own weighted weather list, overriding the worldspace climate inside
// it. This is how Skyrim gives the Reach rain, Winterhold snow, the tundra clear.
struct Region {
  u64 form = 0;
  i32 priority = 0;  // higher wins where regions overlap (REGN RDAT priority)
  std::vector<std::pair<f32, f32>> polygon;        // worldspace XY (game units)
  std::vector<std::pair<WeatherDef, u32>> climate;  // (weather, chance)
};

// The set of weather regions in a worldspace; resolves which one's climate
// applies at a point. Pure geometry, so it is unit-testable without game data.
class RegionWeather {
 public:
  void Add(Region region) { regions_.push_back(std::move(region)); }
  bool empty() const { return regions_.empty(); }
  size_t size() const { return regions_.size(); }
  // The highest-priority region's climate containing (x, y), or null when no
  // weather region covers the point. *out_region gets that region's form (0 if
  // none), so the caller can detect transitions.
  const std::vector<std::pair<WeatherDef, u32>>* ClimateAt(f32 x, f32 y, u64* out_region) const;

 private:
  std::vector<Region> regions_;
};

// Selects the active weather from a climate (a weighted weather list, like a
// Bethesda CLMT) and cross-fades between weathers over time. Stateless and
// deterministic given the seed and the world clock's game time, so it needs no
// per-frame state and replicates in multiplayer by sharing the seed alone.
class WeatherSystem {
 public:
  // (weather, chance) pairs; chance is a relative weight.
  void SetClimate(std::vector<std::pair<WeatherDef, u32>> weighted);
  void set_seed(u64 seed) { seed_ = seed; }
  // Hours one weather holds before it transitions, and the cross-fade length.
  void set_timing(f32 hold_hours, f32 transition_hours) {
    hold_hours_ = hold_hours;
    transition_hours_ = transition_hours;
  }

  bool empty() const { return climate_.empty(); }
  size_t size() const { return climate_.size(); }

  // The blended weather at `game_days` (from the WorldClock).
  WeatherState At(f64 game_days) const;
  // The settled weather, and the cross-fade target (== Current when not fading).
  const WeatherDef& Current(f64 game_days) const;
  const WeatherDef& Target(f64 game_days) const;
  // Cross-fade fraction 0..1 (0 = settled on Current).
  f32 Transition(f64 game_days) const;

 private:
  const WeatherDef& ForSlot(i64 slot) const;
  std::vector<std::pair<WeatherDef, u32>> climate_;
  u32 total_chance_ = 0;
  u64 seed_ = 0;
  f32 hold_hours_ = 5.0f;
  f32 transition_hours_ = 0.6f;
};

}  // namespace rec::weather

#endif  // RECREATION_WEATHER_WEATHER_H_
