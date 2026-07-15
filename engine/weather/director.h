#ifndef RECREATION_WEATHER_DIRECTOR_H_
#define RECREATION_WEATHER_DIRECTOR_H_

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/math.h"
#include "core/types.h"
#include "render/core/settings.h"
#include "weather/weather.h"

namespace rx::audio {
class AudioSystem;
class SoundCatalog;
}  // namespace rx::audio

namespace rx::weather {

// Everything the director needs from the frame, gathered by the caller. The
// timescale (game seconds per real second, the world clock's rate) converts the
// shared game clock into strike-schedule seconds so multiplayer clients that
// share game time also share every lightning strike.
struct Tick {
  f64 game_days = 0.0;      // world clock, drives selection + strike slots
  f32 real_seconds = 0.0f;  // wall-ish clock, drives audio delays
  f32 frame_delta = 0.0f;   // seconds, for the cross-fades and integrators
  f32 timescale = 1.0f;     // game seconds per real second (world clock rate)
  Vec3 anchor{};            // player/camera, engine metres (region + strikes)
  Vec3 listener{};          // audio positioning (usually == anchor)
  bool indoors = false;     // interior cell: no precipitation, flash or strikes
};

// The one place weather happens: owns the selection system, the region
// overrides, the debug/trailer override, the lightning-strike scheduler with
// its thunder-audio queue, the wetness/snow-cover integrators and the
// precipitation/wind audio beds. The engine gathers a Tick, calls Update once
// per frame, and reads the blended state back for sun tinting. Deterministic
// where it matters: weather selection and strike scheduling derive from
// (seed, game time) alone. Without BindAudio the audio paths are inert, so the
// pure logic unit-tests without a device or renderer.
class Director {
 public:
  // The worldspace's default climate + its REGN region overrides. Resets the
  // active-region and cross-fade state (a new game was loaded).
  void SetContent(std::vector<std::pair<WeatherDef, u32>> climate, RegionWeather regions,
                  u64 seed);

  // Debug panel / trailer override: copies *state and pins the weather to it;
  // null clears back to the climate. The pointers below let the debug UI edit
  // the override in place, unchanged from the pre-director wiring.
  void SetOverride(const WeatherState* state);
  bool* debug_override_enable() { return &override_; }
  WeatherState* debug_override_state() { return &override_state_; }

  // Audio, wired once when the game's sound catalog exists. Either null keeps
  // every audio path inert.
  void BindAudio(audio::AudioSystem* audio, const audio::SoundCatalog* catalog);

  // Aurora is Skyrim's; other games gate it off here rather than in the defs.
  void set_aurora_allowed(bool allowed) { aurora_allowed_ = allowed; }
  // The renderer's aerial-perspective baseline the aerosol haze scales.
  void set_ap_base(f32 ap_base) { ap_base_ = ap_base; }
  // RX_LIGHTNING: holds the flash scalar at a fixed level (strikes still run).
  void set_flash_pin(std::optional<f32> pin) { flash_pin_ = pin; }
  // Debug hook: force a strike ~distance_m from the anchor on the next Update.
  void RequestStrike(f32 distance_m) { requested_strike_m_ = distance_m; }

  // Resolves the frame's weather into the renderer's weather + sky settings and
  // services the audio beds / thunder queue. `out` is the weather block inside
  // `sky` (passed separately so tests can hand two plain structs).
  void Update(const Tick& tick, render::WeatherSettings* out, render::RenderSettings* sky);

  // The blended per-frame state (after region cross-fade and override), for the
  // sun tint/dim driving that stays with the engine's day/night code.
  const WeatherState& current() const { return current_; }
  // Whether any weather drives the sky this frame (an empty climate with no
  // override leaves the renderer's settings alone, as demo scenes expect).
  bool active() const { return override_ || !system_.empty(); }

 private:
  // A looping audio bed (rain / wind) whose gain follows the weather. The loop
  // is started once and its gain ramped; it is never restarted while playing.
  struct Bed {
    std::string path;
    u32 voice = 0;
    f32 gain = 0.0f;
  };
  // A thunder clap waiting out its distance delay (speed of sound).
  struct PendingThunder {
    f32 due_real_seconds = 0.0f;
    Vec3 position{};
    f32 gain = 1.0f;
    u64 sound = 0;  // packed sound form, resolved through the catalog when due
  };

  void ResolveRegion(const Tick& tick);
  void UpdateIntegrators(const Tick& tick);
  void UpdateStrikes(const Tick& tick, render::WeatherSettings* out);
  void UpdateAudio(const Tick& tick);
  void UpdateBed(Bed* bed, const std::string& target_path, f32 target_gain, f32 frame_delta);
  // The def whose sounds play right now: the dominant side of the cross-fade.
  const WeatherDef& DominantDef(f64 game_days) const;

  WeatherSystem system_;
  RegionWeather regions_;
  std::vector<std::pair<WeatherDef, u32>> default_climate_;
  u64 seed_ = 0;
  u64 active_region_ = 0;
  // Cross-fade over a few seconds when the region changes, instead of snapping.
  WeatherState region_blend_from_;
  f32 region_blend_t_ = 1.0f;

  bool override_ = false;
  WeatherState override_state_;
  WeatherState current_;

  bool aurora_allowed_ = false;
  f32 ap_base_ = 1.0f;
  std::optional<f32> flash_pin_;

  // Surface response, integrated frame-rate independently (see the .cc).
  f32 wetness_ = 0.0f;
  f32 snow_cover_ = 0.0f;

  // Strike scheduler: the last fired slot of the deterministic schedule, and
  // the active strike's parameters. Slots derive from (seed, game time), so no
  // per-frame randomness accumulates.
  i64 last_fired_slot_ = INT64_MIN;
  Vec3 strike_pos_{};
  u32 strike_seed_ = 0;
  f32 strike_energy_ = 1.0f;
  f64 strike_fire_storm_seconds_ = -1.0e18;  // on the shared storm clock
  // Debug "Strike now": distance of the forced strike, < 0 when none pending;
  // the forced strike runs on the real clock so it works with time frozen.
  f32 requested_strike_m_ = -1.0f;
  f32 manual_strike_real_seconds_ = -1.0e9f;

  audio::AudioSystem* audio_ = nullptr;
  const audio::SoundCatalog* catalog_ = nullptr;
  Bed precip_bed_;
  Bed wind_bed_;
  std::vector<PendingThunder> thunder_queue_;
  bool warned_missing_thunder_ = false;
};

}  // namespace rx::weather

#endif  // RECREATION_WEATHER_DIRECTOR_H_
