#include "weather/director.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "audio/audio_system.h"
#include "audio/sound_catalog.h"
#include "bethesda/form_id.h"
#include "core/log.h"

namespace rx::weather {
namespace {

// Engine metres -> Bethesda units, the streamer's fixed world scale.
constexpr f32 kEngineToGame = 1.0f / 0.01428f;
// Region changes ease in over this many seconds instead of snapping.
constexpr f32 kRegionFadeSeconds = 5.0f;
// A freshly scheduled strike older than this is history (a storm we walked in
// on): its bolt/flash would be invisible, so no thunder is queued either.
constexpr f32 kStrikeStaleSeconds = 5.0f;
// The debug-forced strike runs its visual for this long on the real clock.
constexpr f32 kManualStrikeSeconds = 3.0f;
constexpr f32 kSpeedOfSound = 343.0f;  // m/s, delays the thunder clap
constexpr f32 kTau = 6.28318530718f;

u64 Mix(u64 x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return x;
}

// Hash of (seed, slot, salt) folded to [0, 1). Everything a strike randomises
// comes through here, so the schedule replays identically from the seed.
f32 HashF(u64 seed, i64 slot, u64 salt) {
  const u64 h = Mix(seed ^ (static_cast<u64>(slot) * 0x9e3779b97f4a7c15ULL) ^
                    (salt * 0xbf58476d1ce4e5b9ULL));
  return static_cast<f32>(h >> 40) / 16777216.0f;
}

f32 Clamp01(f32 v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// Frame-rate independent exponential approach: the fraction of the remaining
// distance covered in `dt` at time constant `tau`.
f32 Approach(f32 value, f32 target, f32 dt, f32 tau) {
  if (tau <= 0.0f) return target;
  return value + (target - value) * (1.0f - std::exp(-dt / tau));
}

// The main flash plus a delayed flicker, the envelope the old frame-loop
// scheduler used; age in seconds since the strike.
f32 FlashEnvelope(f32 age) {
  if (age < 0.0f) return 0.0f;
  const f32 a = std::exp(-age * 9.0f);
  const f32 b = age > 0.12f ? 0.65f * std::exp(-(age - 0.12f) * 12.0f) : 0.0f;
  return std::min(1.0f, a + b);
}

bethesda::GlobalFormId Unpack(u64 packed) {
  return bethesda::GlobalFormId{static_cast<u16>(packed >> 32),
                                static_cast<u32>(packed & 0xffffffffu)};
}

const WeatherDef kNeutralDef;

}  // namespace

void Director::SetContent(std::vector<std::pair<WeatherDef, u32>> climate, RegionWeather regions,
                          u64 seed) {
  default_climate_ = climate;
  system_.SetClimate(std::move(climate));
  system_.set_seed(seed);
  seed_ = seed;
  regions_ = std::move(regions);
  active_region_ = 0;
  region_blend_t_ = 1.0f;
  last_fired_slot_ = INT64_MIN;
}

void Director::SetOverride(const WeatherState* state) {
  override_ = state != nullptr;
  if (state) override_state_ = *state;
}

void Director::BindAudio(audio::AudioSystem* audio, const audio::SoundCatalog* catalog) {
  audio_ = audio;
  catalog_ = catalog;
}

const WeatherDef& Director::DominantDef(f64 game_days) const {
  if (system_.empty()) return kNeutralDef;
  return system_.Transition(game_days) < 0.5f ? system_.Current(game_days)
                                              : system_.Target(game_days);
}

void Director::ResolveRegion(const Tick& tick) {
  // The REGN area the player stands in overrides the worldspace climate
  // (Skyrim's per-region weather). Swapped only when the active region changes;
  // skipped while an override pins the weather.
  if (override_ || regions_.empty()) return;
  u64 region = 0;
  const auto* climate = regions_.ClimateAt(tick.anchor.x * kEngineToGame,
                                           -tick.anchor.z * kEngineToGame, &region);
  if (region == active_region_) return;
  // Capture the weather we are leaving (old climate) to cross-fade from.
  region_blend_from_ = system_.empty() ? WeatherState{} : system_.At(tick.game_days);
  region_blend_t_ = 0.0f;
  active_region_ = region;
  system_.SetClimate(climate ? *climate : default_climate_);
  RX_INFO("weather: region {:x} ({} weathers)", region,
          climate ? climate->size() : default_climate_.size());
}

void Director::UpdateIntegrators(const Tick& tick) {
  // The world outside keeps soaking while the player is in a dungeon, so the
  // integrators follow the weather itself, not the indoor-masked output.
  const f32 rain = current_.snow ? 0.0f : current_.precipitation;
  const f32 snowfall = current_.snow ? current_.precipitation : 0.0f;

  // Wetness: full rain soaks the ground in ~45 s (tau 15 s reaches ~95%),
  // lighter rain proportionally slower; drying takes ~5 min under cloud and
  // ~3 min once the sky clears.
  if (rain > 0.01f) {
    wetness_ = Approach(wetness_, 1.0f, tick.frame_delta, 15.0f / rain);
  } else {
    const f32 dry_tau = current_.cloud_coverage < 0.3f ? 60.0f : 100.0f;
    wetness_ = Approach(wetness_, 0.0f, tick.frame_delta, dry_tau);
  }

  // Snow cover: a steady fall blankets in ~3 min, melts over ~12 min. Starting
  // from zero it stays exactly zero until it actually snows.
  if (snowfall > 0.01f) {
    snow_cover_ = Approach(snow_cover_, 1.0f, tick.frame_delta, 60.0f / snowfall);
  } else {
    snow_cover_ = Approach(snow_cover_, 0.0f, tick.frame_delta, 240.0f);
  }
  wetness_ = Clamp01(wetness_);
  snow_cover_ = Clamp01(snow_cover_);
}

void Director::UpdateStrikes(const Tick& tick, render::WeatherSettings* out) {
  // Debug "Strike now": synthesized on the real clock so it works everywhere
  // (time frozen, no storm). Direction hashed off the request time.
  if (requested_strike_m_ >= 0.0f) {
    const f32 dist = requested_strike_m_;
    requested_strike_m_ = -1.0f;
    manual_strike_real_seconds_ = tick.real_seconds;
    const u64 h = Mix(static_cast<u64>(tick.real_seconds * 977.0f) ^ seed_);
    const f32 az = static_cast<f32>(h >> 40) / 16777216.0f * kTau;
    strike_pos_ = {tick.anchor.x + std::cos(az) * dist, tick.anchor.y,
                   tick.anchor.z + std::sin(az) * dist};
    strike_seed_ = static_cast<u32>(h);
    strike_energy_ = 1.0f;
    if (audio_ && catalog_) {
      const WeatherDef& def = DominantDef(tick.game_days);
      if (!def.sound_thunder.empty())
        thunder_queue_.push_back({tick.real_seconds + dist / kSpeedOfSound, strike_pos_,
                                  1.0f / (1.0f + dist / 300.0f),
                                  def.sound_thunder[h % def.sound_thunder.size()]});
    }
  }

  const bool storm = active() && !tick.indoors && current_.thunder;
  f32 age = -1.0f;
  if (storm) {
    // Strikes fire on a deterministic schedule shared by everyone who shares
    // the world clock: game time converted to real-equivalent "storm seconds",
    // sliced into thunder-period slots, one strike per slot at a hashed phase.
    const f32 period = current_.thunder_period > 0.5f ? current_.thunder_period : 10.0f;
    const f64 storm_seconds =
        tick.game_days * 86400.0 / static_cast<f64>(std::max(tick.timescale, 1e-3f));
    const i64 slot = static_cast<i64>(std::floor(storm_seconds / period));
    auto fire_time = [&](i64 s) {
      return static_cast<f64>(s) * period + HashF(seed_, s, 1) * period * 0.6;
    };
    const i64 fired = storm_seconds >= fire_time(slot) ? slot : slot - 1;
    const f64 fired_age = storm_seconds - fire_time(fired);
    if (fired != last_fired_slot_) {
      last_fired_slot_ = fired;
      // Only a fresh strike spawns a bolt and thunder; walking into a storm
      // mid-slot must not replay one that visually happened seconds ago.
      if (fired_age < kStrikeStaleSeconds) {
        const f32 az = HashF(seed_, fired, 2) * kTau;
        const f32 dist = 250.0f + HashF(seed_, fired, 3) * 1250.0f;  // 250..1500 m
        strike_pos_ = {tick.anchor.x + std::cos(az) * dist, tick.anchor.y,
                       tick.anchor.z + std::sin(az) * dist};
        strike_seed_ = static_cast<u32>(Mix(seed_ ^ static_cast<u64>(fired)));
        // Distant strikes vary; anything close overhead hits with full energy.
        strike_energy_ = dist < 400.0f ? 1.0f : 0.6f + 0.4f * HashF(seed_, fired, 4);
        strike_fire_storm_seconds_ = fire_time(fired);
        if (audio_ && catalog_) {
          const WeatherDef& def = DominantDef(tick.game_days);
          if (!def.sound_thunder.empty()) {
            thunder_queue_.push_back(
                {tick.real_seconds + dist / kSpeedOfSound, strike_pos_,
                 1.0f / (1.0f + dist / 300.0f),
                 def.sound_thunder[Mix(seed_ ^ static_cast<u64>(fired) ^ 7) %
                                   def.sound_thunder.size()]});
          } else if (!warned_missing_thunder_) {
            warned_missing_thunder_ = true;
            RX_INFO("weather: thundery weather carries no thunder sounds (SNAM)");
          }
        }
      }
    }
    age = static_cast<f32>(storm_seconds - strike_fire_storm_seconds_);
  }

  // The forced strike wins while it runs (real clock, so it survives pauses).
  const f32 manual_age = tick.real_seconds - manual_strike_real_seconds_;
  if (manual_age >= 0.0f && manual_age < kManualStrikeSeconds && !tick.indoors) age = manual_age;

  const bool strike_active = age >= 0.0f && age < kStrikeStaleSeconds;
  out->lightning = strike_active ? FlashEnvelope(age) : 0.0f;
  out->strike_age = strike_active ? age : -1.0f;
  out->strike_pos = strike_pos_;
  out->strike_seed = strike_seed_;
  out->strike_energy = strike_energy_;
  // RX_LIGHTNING holds the flash at a fixed level (testing the brief strike).
  if (flash_pin_) out->lightning = *flash_pin_;
}

void Director::UpdateBed(Bed* bed, const std::string& target_path, f32 target_gain,
                         f32 frame_delta) {
  // A changed source cross-fades: the old loop fades out on its own while the
  // fresh one ramps up from silence. The same loop is never restarted.
  if (target_path != bed->path) {
    if (bed->voice) {
      audio_->Stop(bed->voice, 1.5f);
      bed->voice = 0;
    }
    bed->path = target_path;
    bed->gain = 0.0f;
  }
  if (bed->path.empty()) return;
  // Ramp the gain toward the weather over ~2 s, full scale.
  const f32 step = frame_delta * 0.5f;
  bed->gain += std::clamp(target_gain - bed->gain, -step, step);
  if (!bed->voice) {
    if (target_gain > 0.02f) {
      audio::PlayParams params;
      params.gain = 0.0f;  // the ramp below brings it in
      params.positional = false;
      bed->voice = audio_->PlayLoop(bed->path, params);
      if (!bed->voice) bed->path.clear();  // undecodable: stop retrying this file
    }
  } else {
    audio_->SetVoiceGain(bed->voice, bed->gain);
    if (target_gain <= 0.02f && bed->gain <= 0.02f) {
      audio_->Stop(bed->voice, 1.0f);
      bed->voice = 0;
    }
  }
}

void Director::UpdateAudio(const Tick& tick) {
  if (!audio_ || !catalog_) return;

  // Rain/wind beds follow the blended weather; indoors both fall silent (the
  // interior systems own the soundscape there).
  const WeatherDef& def = DominantDef(tick.game_days);
  std::string precip_path, wind_path;
  f32 precip_gain = 0.0f, wind_gain = 0.0f;
  if (active() && !tick.indoors) {
    if (def.sound_precip && current_.precipitation > 0.01f) {
      precip_path = catalog_->PathFor(Unpack(def.sound_precip));
      precip_gain = Clamp01(current_.precipitation) * 0.8f;
    }
    if (def.sound_wind && current_.wind > 3.0f) {
      wind_path = catalog_->PathFor(Unpack(def.sound_wind));
      wind_gain = Clamp01(current_.wind / 25.0f) * 0.6f;
    }
  }
  UpdateBed(&precip_bed_, precip_path, precip_gain, tick.frame_delta);
  UpdateBed(&wind_bed_, wind_path, wind_gain, tick.frame_delta);

  // Thunder claps whose sound has finished travelling. Missing files skip
  // quietly (logged once): distant mods can strip sounds without consequence.
  for (size_t i = 0; i < thunder_queue_.size();) {
    if (tick.real_seconds < thunder_queue_[i].due_real_seconds) {
      ++i;
      continue;
    }
    const PendingThunder clap = thunder_queue_[i];
    thunder_queue_.erase(thunder_queue_.begin() + static_cast<ptrdiff_t>(i));
    const std::string path = catalog_->PathFor(Unpack(clap.sound));
    if (path.empty() || !audio_->HasAsset(path)) {
      if (!warned_missing_thunder_) {
        warned_missing_thunder_ = true;
        RX_INFO("weather: thunder sound {:x} unresolvable, skipping claps", clap.sound);
      }
      continue;
    }
    audio::PlayParams params;
    params.gain = clap.gain;
    params.positional = true;
    audio_->PlayAt(path, clap.position, params);
  }
}

void Director::Update(const Tick& tick, render::WeatherSettings* out,
                      render::RenderSettings* sky) {
  ResolveRegion(tick);

  // The frame's blended state: the override verbatim, else the climate's
  // deterministic pick, eased across region changes.
  if (override_) {
    current_ = override_state_;
  } else if (!system_.empty()) {
    current_ = system_.At(tick.game_days);
    if (region_blend_t_ < 1.0f) {
      region_blend_t_ = std::min(1.0f, region_blend_t_ + tick.frame_delta / kRegionFadeSeconds);
      const f32 s = region_blend_t_ * region_blend_t_ * (3.0f - 2.0f * region_blend_t_);
      current_ = Lerp(region_blend_from_, current_, s);
    }
  } else {
    current_ = WeatherState{};
  }

  UpdateIntegrators(tick);

  if (active()) {
    sky->cloud_coverage = current_.cloud_coverage;
    sky->aerial_perspective = ap_base_ * (1.0f + current_.aerosol * 2.0f);
    // No rain/snow indoors: interior cells have no sky overhead. The surface
    // integrals still apply -- the world outside stayed wet.
    out->precipitation = tick.indoors ? 0.0f : current_.precipitation;
    out->snow = current_.snow;
    out->wind_yaw = current_.wind_yaw;
    out->wind_speed = current_.wind;
    out->gustiness = current_.gustiness;
    out->wetness = wetness_;
    out->snow_cover = snow_cover_;
    out->aurora = aurora_allowed_ && current_.aurora > 0.01f;
    out->aurora_intensity = Clamp01(current_.aurora);
  }

  UpdateStrikes(tick, out);
  UpdateAudio(tick);
}

}  // namespace rx::weather
