#ifndef RECREATION_RUNTIME_TRAILER_H_
#define RECREATION_RUNTIME_TRAILER_H_

#include <string>
#include <vector>

#include "core/types.h"
#include "weather/weather.h"

namespace rec {

// Which rendering path the trailer is showing off this moment. The badge text
// names it on screen; the engine maps it onto the renderer's feature flags.
enum class TrailerRenderMode : u8 { kRaster, kRayTracing, kPathTracing };

const char* TrailerRenderModeLabel(TrailerRenderMode mode);

// One on-screen frame of trailer chrome: cinematic letterbox, a black wash for
// the intro/outro fades, a lower-third location title, a render-mode badge with
// the live weather, and a centered intro card. Every alpha is 0..1 so the drawer
// just multiplies it in. Pure data, so the timeline is unit-testable with no
// renderer.
struct TrailerOverlay {
  bool active = false;
  f32 letterbox = 0.0f;  // 0..1 of the cinematic bars' full height
  f32 fade = 0.0f;       // 0..1 black wash (fade in at the start, out at the end)

  std::string title;     // location title (game), e.g. "SKYRIM"
  std::string subtitle;  // worldspace / tagline under it
  f32 title_alpha = 0.0f;

  std::string badge;        // render-mode label, e.g. "PATH TRACING  REFERENCE"
  std::string weather_tag;  // live weather name under the badge
  f32 badge_alpha = 0.0f;

  std::string intro_title;     // centered opening card
  std::string intro_subtitle;
  f32 intro_alpha = 0.0f;

  // Small loading screen shown over black while a freshly cut-to game streams in
  // (set by the engine, not the timeline). loading_label names the incoming game.
  bool loading = false;
  std::string loading_label;
};

struct TrailerState {
  weather::WeatherState weather;
  TrailerRenderMode mode = TrailerRenderMode::kRayTracing;
  TrailerOverlay overlay;
};

// Scripts the trailer over the showcase flythrough's timeline: cycles weather
// and render mode on their own cadences (so the full range shows even over a
// single map) and fades a location title in as the camera reaches each region.
// Pure timeline math; the engine samples At(t) each frame and applies it to the
// weather override, the render settings and the on-screen overlay.
class TrailerDirector {
 public:
  // A location title window, one per showcase region, in flythrough seconds.
  struct Beat {
    f32 start = 0.0f;
    f32 end = 0.0f;
    std::string title;
    std::string subtitle;
  };

  void set_intro(std::string title, std::string subtitle) {
    intro_title_ = std::move(title);
    intro_subtitle_ = std::move(subtitle);
  }
  void AddBeat(Beat beat) { beats_.push_back(std::move(beat)); }
  void set_duration(f32 seconds) { duration_ = seconds; }

  bool empty() const { return beats_.empty(); }
  f32 duration() const { return duration_; }

  TrailerState At(f32 t) const;

  // The index of the beat (showcase region) whose window contains t. Clamped to
  // [0, beats-1]. The engine uses it to keep only that game's cells resident.
  int ActiveBeatIndex(f32 t) const;

 private:
  std::string intro_title_ = "RECREATION";
  std::string intro_subtitle_;
  std::vector<Beat> beats_;
  f32 duration_ = 0.0f;
};

}  // namespace rec

#endif  // RECREATION_RUNTIME_TRAILER_H_
