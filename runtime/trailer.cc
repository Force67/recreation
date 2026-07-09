#include "trailer.h"

#include <algorithm>
#include <cmath>

namespace rx {

const char* TrailerRenderModeLabel(TrailerRenderMode mode) {
  switch (mode) {
    case TrailerRenderMode::kRaster: return "RASTERIZED";
    case TrailerRenderMode::kRayTracing: return "RAY TRACING  ULTRA";
    case TrailerRenderMode::kPathTracing: return "PATH TRACING  REFERENCE";
  }
  return "";
}

namespace {

// Timeline cadences (seconds). Weather and render mode cycle independently of
// the camera path so the full range shows even over a single worldspace.
constexpr f32 kIntro = 4.0f;  // opening title card hold
constexpr f32 kFade = 1.0f;   // fade-from/to-black at the ends

constexpr f32 kWeatherHold = 9.0f;  // each weather settles this long...
constexpr f32 kWeatherFade = 3.0f;  // ...then cross-fades over this
constexpr f32 kModeHold = 12.0f;    // each render mode holds this long
constexpr f32 kModeFade = 0.5f;     // badge dip on a mode swap
constexpr f32 kCutIn = 0.6f;        // fade to black before a game cut...
constexpr f32 kCutOut = 1.6f;       // ...then back, giving the next game time to stream

f32 Clamp01(f32 x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
f32 Smooth(f32 x) {
  x = Clamp01(x);
  return x * x * (3.0f - 2.0f * x);
}

// Trapezoid envelope: 0 before `start`, ramps up over `in`, 1 across the middle,
// ramps down over `out`, 0 after `end`.
f32 Envelope(f32 t, f32 start, f32 end, f32 in, f32 out) {
  if (t <= start || t >= end) return 0.0f;
  return std::min(Smooth((t - start) / std::max(in, 1e-3f)),
                  Smooth((end - t) / std::max(out, 1e-3f)));
}

weather::WeatherState KindState(weather::WeatherDef::Kind kind) {
  weather::WeatherDef def;
  def.kind = kind;
  def.DeriveFromKind();
  return weather::ToState(def);
}

const char* WeatherName(weather::WeatherDef::Kind kind) {
  switch (kind) {
    case weather::WeatherDef::Kind::kPleasant: return "CLEAR";
    case weather::WeatherDef::Kind::kCloudy: return "OVERCAST";
    case weather::WeatherDef::Kind::kRainy: return "STORM";
    case weather::WeatherDef::Kind::kSnow: return "BLIZZARD";
  }
  return "";
}

// Looping showcase weather order: clear -> overcast -> storm -> blizzard.
constexpr weather::WeatherDef::Kind kWeatherCycle[] = {
    weather::WeatherDef::Kind::kPleasant,
    weather::WeatherDef::Kind::kCloudy,
    weather::WeatherDef::Kind::kRainy,
    weather::WeatherDef::Kind::kSnow,
};
constexpr int kWeatherCount = 4;

// The two showcase render paths, alternated so the badge names what is on
// screen: ray tracing, then the reference path tracer, then back.
constexpr TrailerRenderMode kModeCycle[] = {
    TrailerRenderMode::kRayTracing,
    TrailerRenderMode::kPathTracing,
};
constexpr int kModeCount = 2;

int Wrap(int i, int n) { return ((i % n) + n) % n; }

}  // namespace

TrailerState TrailerDirector::At(f32 t) const {
  TrailerState st;
  t = std::max(0.0f, t);
  // No real end during tests / single-shot use: treat as effectively endless so
  // the letterbox holds open instead of snapping shut.
  const f32 end = duration_ > 0.0f ? duration_ : t + 1.0e6f;
  TrailerOverlay& o = st.overlay;
  o.active = true;

  // --- Weather: continuous, cross-faded cycle ---
  {
    const f32 period = kWeatherHold + kWeatherFade;
    const int idx = static_cast<int>(std::floor(t / period));
    const f32 local = t - static_cast<f32>(idx) * period;
    const auto cur = kWeatherCycle[Wrap(idx, kWeatherCount)];
    const auto nxt = kWeatherCycle[Wrap(idx + 1, kWeatherCount)];
    if (local <= kWeatherHold) {
      st.weather = KindState(cur);
      o.weather_tag = WeatherName(cur);
    } else {
      const f32 f = Smooth((local - kWeatherHold) / kWeatherFade);
      st.weather = weather::Lerp(KindState(cur), KindState(nxt), f);
      o.weather_tag = WeatherName(f < 0.5f ? cur : nxt);
    }
  }

  // --- Render mode: stepped cycle, begins after the intro card ---
  f32 mode_dip = 1.0f;
  {
    const f32 mt = std::max(0.0f, t - kIntro);
    const int idx = static_cast<int>(std::floor(mt / kModeHold));
    st.mode = kModeCycle[Wrap(idx, kModeCount)];
    o.badge = TrailerRenderModeLabel(st.mode);
    // Dip the badge alpha briefly at each swap so the label reads as changing.
    const f32 ml = mt - static_cast<f32>(idx) * kModeHold;
    mode_dip = std::min(Smooth(ml / kModeFade), Smooth((kModeHold - ml) / kModeFade));
  }

  // --- Chrome alphas ---
  // Letterbox eases in over the first second, holds, eases out at the very end.
  o.letterbox = Envelope(t, 0.0f, end, kFade, kFade);
  // Black wash: full at t=0 (reveal), again across the final second (exit), and a
  // quick dip through black at each game cut (the boundary between beats), which
  // both reads as a scene change and hides the unload/stream-in of the next game.
  const f32 fade_in = 1.0f - Smooth(t / kFade);
  const f32 fade_out = duration_ > 0.0f ? Smooth((t - (duration_ - kFade)) / kFade) : 0.0f;
  o.fade = std::max(fade_in, fade_out);
  for (size_t k = 1; k < beats_.size(); ++k) {
    const f32 b = beats_[k].start;
    const f32 cut = t <= b ? Smooth((t - (b - kCutIn)) / kCutIn) : Smooth((b + kCutOut - t) / kCutOut);
    o.fade = std::max(o.fade, cut);
  }

  // Intro card: centered title, alive only during the opening hold.
  o.intro_title = intro_title_;
  o.intro_subtitle = intro_subtitle_;
  o.intro_alpha = Envelope(t, 0.0f, kIntro, kFade, 0.8f);

  // Render-mode badge: up once the intro clears, riding the letterbox and the
  // per-swap dip.
  o.badge_alpha = t < kIntro ? 0.0f : o.letterbox * mode_dip;

  // Location lower-third: the beat whose window contains t, suppressed under the
  // intro card so the two never overlap.
  if (t >= kIntro) {
    for (const Beat& b : beats_) {
      if (t < b.start || t >= b.end) continue;
      const f32 a = Envelope(t, b.start, b.end, 1.2f, 1.2f);
      if (a <= o.title_alpha) continue;
      o.title = b.title;
      o.subtitle = b.subtitle;
      o.title_alpha = a;
    }
  }

  return st;
}

int TrailerDirector::ActiveBeatIndex(f32 t) const {
  if (beats_.empty()) return 0;
  for (size_t k = 0; k < beats_.size(); ++k)
    if (t < beats_[k].end) return static_cast<int>(k);
  return static_cast<int>(beats_.size()) - 1;
}

}  // namespace rx
