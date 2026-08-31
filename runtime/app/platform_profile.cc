#include "runtime/app/platform_profile.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

#include <base/option.h>

#include "core/log.h"
#include "render/core/settings_ini.h"

namespace rx {
namespace {

std::string Trim(std::string_view sv) {
  size_t b = 0, e = sv.size();
  while (b < e && std::isspace(static_cast<unsigned char>(sv[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(sv[e - 1]))) --e;
  return std::string(sv.substr(b, e - b));
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Sets the registered base::Option named `name`. False when no option carries
// that name, or when the value does not parse as that option's type.
bool SetOption(const std::string& name, const std::string& value) {
  bool applied = false;
  base::OptionBase::VisitAll([&](const base::OptionBase* registered) {
    if (applied || name != registered->name()) return;
    // The chain hands out const pointers; the options themselves are mutable
    // globals, so writing through one here is well-defined (same reasoning as
    // base::InitOptionsFromEnv).
    applied = const_cast<base::OptionBase*>(registered)->SetFromString(value.c_str());
  });
  return applied;
}

// --- [engine] ---------------------------------------------------------------

bool ApplyEngineKey(const std::string& key, const std::string& value, EngineConfig& config) {
  if (key == "grass_density") {
    config.grass_density = std::strtof(value.c_str(), nullptr);
    return true;
  }
  if (key == "max_quest_scripts") {
    config.max_quest_scripts = std::atoi(value.c_str());
    return true;
  }
  if (key == "preset") {
    // Names match rx's ParsePreset ("steamdeck", "low", "console", ...); an
    // unknown name resolves to kAuto there, which would silently drop the
    // profile's intent, so reject it here instead.
    render::QualityPreset parsed = render::ParsePreset(value);
    if (parsed == render::QualityPreset::kAuto && value != "auto") return false;
    config.preset = parsed;
    return true;
  }
  return false;
}

}  // namespace

void PlatformProfile::ApplyRenderSettings(render::RenderSettings& s) const {
  if (render_ini.empty()) return;
  const int applied = render::ApplyIni(std::string_view(render_ini.c_str(), render_ini.size()), s);
  RX_INFO("platform profile: {} render keys applied after the tier preset", applied);
}

void ApplyPlatformProfile(std::string_view text, EngineConfig& config, PlatformProfile* out) {
  std::string section;
  std::string render_section;
  std::istringstream in{std::string(text)};
  std::string raw;

  while (std::getline(in, raw)) {
    const std::string line = Trim(raw);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    if (line.front() == '[' && line.back() == ']') {
      section = Lower(Trim(std::string_view(line).substr(1, line.size() - 2)));
      continue;
    }

    const size_t eq = line.find('=');
    if (eq == std::string::npos) {
      RX_WARN("platform profile: ignoring malformed line '{}'", line);
      continue;
    }
    const std::string key = Lower(Trim(std::string_view(line).substr(0, eq)));
    const std::string value = Trim(std::string_view(line).substr(eq + 1));

    if (section == "render") {
      // Kept verbatim; rx owns these keys and applies them after the tier
      // preset has rebuilt the settings.
      render_section += key;
      render_section += " = ";
      render_section += value;
      render_section += '\n';
    } else if (section == "engine") {
      if (ApplyEngineKey(key, value, config)) {
        ++out->engine_keys;
      } else {
        RX_WARN("platform profile: unknown [engine] key '{}' (or bad value '{}')", key, value);
      }
    } else if (section == "options") {
      if (SetOption(key, value)) {
        ++out->option_keys;
      } else {
        RX_WARN("platform profile: no registered option '{}' (or bad value '{}')", key, value);
      }
    } else {
      RX_WARN("platform profile: key '{}' outside a known section", key);
    }
  }

  out->render_ini = base::String(render_section.c_str());
}

bool LoadPlatformProfile(const std::filesystem::path& path, EngineConfig& config,
                         PlatformProfile* out) {
  std::ifstream file(path);
  if (!file) return false;
  std::ostringstream buffer;
  buffer << file.rdbuf();
  ApplyPlatformProfile(buffer.str(), config, out);
  RX_INFO("platform profile: {} ({} engine, {} option keys)", path.string(), out->engine_keys,
          out->option_keys);
  return true;
}

std::filesystem::path FindPlatformProfile(std::string_view name) {
  const std::string file = std::string(name) + ".ini";
  std::error_code ec;
  if (const char* dir = std::getenv("REC_PROFILE_DIR")) {
    std::filesystem::path candidate = std::filesystem::path(dir) / file;
    if (std::filesystem::exists(candidate, ec)) return candidate;
  }
  std::filesystem::path candidate = std::filesystem::path("runtime/app/profiles") / file;
  if (std::filesystem::exists(candidate, ec)) return candidate;
  return {};
}

}  // namespace rx
