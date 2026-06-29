#ifndef RECREATION_RENDER_SETTINGS_INI_H_
#define RECREATION_RENDER_SETTINGS_INI_H_

#include <filesystem>
#include <string>
#include <string_view>

#include "render/core/settings.h"

namespace rec::render {

// Text (de)serialization of RenderSettings as a flat INI: "key = value" lines
// grouped under cosmetic [section] headers, enums written as lowercase names.
// This backs the editable platform presets in engine/render/presets and the
// load/save controls in the debug ui.
//
// Only persistent quality/performance knobs are covered. Scene state owned by
// other systems is deliberately excluded so a preset never fights them:
//   - sun_direction / sun_intensity / sun_color / ambient  (day/night clock)
//   - precipitation / precip_snow / aurora / lightning      (weather system)
//   - color_grade                                           (artistic / env)
//   - debug_view / wireframe / path_trace_recon_debug       (debug overlays)
// ApplyIni leaves any field whose key is absent untouched, so partial files and
// the excluded fields keep their incoming value.

// Serializes the covered fields of `s` to INI text.
std::string SettingsToIni(const RenderSettings& s);

// Overlays recognized "key = value" lines from `text` onto `s`. Section headers,
// blank lines and ; / # comments are ignored; unknown keys are skipped. Returns
// the number of keys applied.
int ApplyIni(std::string_view text, RenderSettings& s);

// Reads a preset file and overlays it onto `s` (see ApplyIni). False if the file
// cannot be opened.
bool LoadSettingsIni(const std::filesystem::path& path, RenderSettings& s);

// Writes SettingsToIni(s) to `path`. False on a write error.
bool SaveSettingsIni(const std::filesystem::path& path, const RenderSettings& s);

}  // namespace rec::render

#endif  // RECREATION_RENDER_SETTINGS_INI_H_
