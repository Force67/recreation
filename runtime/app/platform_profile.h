#ifndef RECREATION_APP_PLATFORM_PROFILE_H_
#define RECREATION_APP_PLATFORM_PROFILE_H_

#include <filesystem>
#include <string_view>

#include "render/core/settings.h"
#include "runtime/app/engine_context.h"

namespace rx {

// A recreation-level platform profile: one INI carrying the game and engine
// knobs a target needs, layered on top of the renderer tier rx already ships in
// engine/render/presets. rx's presets describe what the *gpu* can afford;
// these describe what the *game* should do on that machine.
//
// Each section is handed to the layer that owns those fields:
//   [engine]   EngineConfig     grass density, quest cap, quality tier
//   [options]  base::Option     any registered option, by its registered name
//                               (win.width, stream.budget_ms, touch.emits_mouse)
//   [render]   RenderSettings   forwarded verbatim to rx's ApplyIni
//
// [render] cannot be applied when the file is read: Host::ApplyRenderPreset
// rebuilds the settings wholesale from the quality tier once the device caps
// are known, discarding anything written before it. The section's text is kept
// here instead and replayed through AppConfig::tune_settings, which the host
// runs after the preset and after every env carry-through.
//
// Precedence ends up default < profile < environment, since the host populates
// options from the environment after main() has applied the profile, and an
// unset variable never overwrites.
//
// Absent keys keep their incoming value, so a profile is an override on top of
// whatever the command line and auto-detection already produced.
struct PlatformProfile {
  base::String render_ini;  // the [render] section verbatim, replayed post-preset
  int engine_keys = 0;
  int option_keys = 0;

  // Overlays the captured [render] section onto `s`. Bind into
  // AppConfig::tune_settings so the host applies it at the right moment.
  void ApplyRenderSettings(render::RenderSettings& s) const;
};

// Overlays `text` onto `config` and `out`. Section headers, blank lines and
// ; / # comments are ignored; unrecognized keys are logged and skipped.
void ApplyPlatformProfile(std::string_view text, EngineConfig& config, PlatformProfile* out);

// Reads a profile file and overlays it (see ApplyPlatformProfile). False when
// the file cannot be opened.
bool LoadPlatformProfile(const std::filesystem::path& path, EngineConfig& config,
                         PlatformProfile* out);

// Resolves a profile name ("steamdeck") to a file, searching $REC_PROFILE_DIR
// then runtime/app/profiles beside the working directory. Empty when no such
// file exists.
std::filesystem::path FindPlatformProfile(std::string_view name);

}  // namespace rx

#endif  // RECREATION_APP_PLATFORM_PROFILE_H_
