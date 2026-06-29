// trailertest: the trailer director's pure timeline. Checks that weather and
// render mode cycle on their own cadences, the location title fades in only
// inside its region window (and never under the intro card), and the chrome
// alphas (letterbox / fade / intro) behave at the ends. No game data, so it runs
// in the default ctest gate.
#include <cstdio>

#include "../../runtime/trailer.h"

using rec::TrailerDirector;
using rec::TrailerRenderMode;
using rec::TrailerState;

int main() {
  int failures = 0;
  auto check = [&](const char* what, bool ok) {
    std::printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
  };

  TrailerDirector dir;
  check("empty before any beats", dir.empty());

  dir.set_intro("RECREATION", "A BETHESDA ENGINE, REIMAGINED");
  dir.AddBeat({0.0f, 20.0f, "SKYRIM", "THE ELDER SCROLLS V"});
  dir.AddBeat({20.0f, 40.0f, "FALLOUT 4", "THE COMMONWEALTH"});
  dir.set_duration(40.0f);

  check("not empty with beats", !dir.empty());
  check("duration is set", dir.duration() == 40.0f);

  // Overlay is always active mid-flight.
  check("overlay active mid-flight", dir.At(10.0f).overlay.active);

  // Negative / past-end times don't crash and stay clamped.
  (void)dir.At(-5.0f);
  (void)dir.At(1000.0f);
  check("past-end sample is safe", dir.At(1000.0f).overlay.active);

  // --- Chrome at the ends ---
  check("fades from black at t=0", dir.At(0.0f).overlay.fade > 0.9f);
  check("revealed after the fade-in", dir.At(2.0f).overlay.fade < 0.1f);
  check("fades to black at the end", dir.At(39.9f).overlay.fade > 0.8f);
  check("letterbox closed at t=0", dir.At(0.0f).overlay.letterbox < 0.2f);
  check("letterbox open mid-flight", dir.At(10.0f).overlay.letterbox > 0.9f);

  // --- Intro card ---
  check("intro card up early", dir.At(2.0f).overlay.intro_alpha > 0.5f);
  check("intro card gone later", dir.At(10.0f).overlay.intro_alpha < 0.01f);
  check("intro carries the title text", dir.At(2.0f).overlay.intro_title == "RECREATION");

  // --- Location lower-third ---
  check("no location title under the intro", dir.At(1.0f).overlay.title_alpha < 0.01f);
  check("skyrim title in its window", dir.At(10.0f).overlay.title == "SKYRIM");
  check("skyrim subtitle in its window",
        dir.At(10.0f).overlay.subtitle == "THE ELDER SCROLLS V");
  check("fallout title in its window", dir.At(30.0f).overlay.title == "FALLOUT 4");
  check("location title actually visible", dir.At(30.0f).overlay.title_alpha > 0.5f);

  // --- Weather cycle: clear -> overcast -> storm -> blizzard ---
  // Clear opens with little cloud cover; later weathers are heavier and (snow)
  // flip the snow flag.
  const float clear_cover = dir.At(0.5f).weather.cloud_coverage;
  const float overcast_cover = dir.At(13.0f).weather.cloud_coverage;  // into slot 1
  check("weather opens clear", clear_cover < 0.3f);
  check("weather grows overcast", overcast_cover > clear_cover);
  check("clear is not snowing", !dir.At(0.5f).weather.snow);
  // Slot 3 (blizzard) lands around t = 3 * (9 + 3) = 36s.
  check("blizzard slot snows", dir.At(37.0f).weather.snow);
  check("weather tag named at t=0", dir.At(0.5f).overlay.weather_tag == std::string("CLEAR"));

  // --- Render mode cycle: ray tracing, then path tracer, starting post-intro ---
  // kIntro = 4, kModeHold = 12: slot 0 ~ [4,16), slot 1 ~ [16,28), slot 0 ~ [28,40).
  check("ray tracing first", dir.At(8.0f).mode == TrailerRenderMode::kRayTracing);
  check("path tracing second", dir.At(20.0f).mode == TrailerRenderMode::kPathTracing);
  check("ray tracing third", dir.At(32.0f).mode == TrailerRenderMode::kRayTracing);
  check("badge names path tracing",
        dir.At(20.0f).overlay.badge == std::string(rec::TrailerRenderModeLabel(
                                           TrailerRenderMode::kPathTracing)));

  // --- Multi-game: active-beat index drives which map stays resident ---
  check("active beat 0 in first window", dir.ActiveBeatIndex(10.0f) == 0);
  check("active beat 1 in second window", dir.ActiveBeatIndex(30.0f) == 1);
  check("active beat clamps below 0", dir.ActiveBeatIndex(-5.0f) == 0);
  check("active beat clamps past end", dir.ActiveBeatIndex(100.0f) == 1);

  // --- Game cut: a black dip at the beat boundary hides the unload/stream-in ---
  check("fade spikes at the game cut", dir.At(20.0f).overlay.fade > 0.9f);
  check("no fade mid-beat (away from cut)", dir.At(13.0f).overlay.fade < 0.1f);
  check("fade clears after the cut", dir.At(22.0f).overlay.fade < 0.3f);

  // A single-map trailer has no internal cut: fade stays clean mid-flight.
  TrailerDirector solo;
  solo.AddBeat({0.0f, 20.0f, "SKYRIM", "THE ELDER SCROLLS V"});
  solo.set_duration(20.0f);
  check("single map: no mid-flight cut", solo.At(10.0f).overlay.fade < 0.1f);

  std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "passed", failures);
  return failures ? 1 : 0;
}
