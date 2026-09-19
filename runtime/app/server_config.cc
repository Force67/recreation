#include "runtime/app/server_config.h"

#include <fstream>
#include <sstream>
#include <string>

namespace rx {
namespace {

// The settings a config file can carry, and the flag each stands for. Keeping
// the mapping here and the application in the argument parser means a setting
// and its flag can never drift apart.
struct Setting {
  const char* key;
  const char* flag;
  bool takes_value;
};

constexpr Setting kSettings[] = {
    {"name", "--server-name", true},        {"port", "--port", true},
    {"max_clients", "--max-clients", true}, {"data_dir", "--data-dir", true},
    {"plugins", "--plugins", true},         {"game", "--game", true},
    {"mods_dir", "--mods-dir", true},       {"masterlist", "--masterlist", true},
    {"private", "--private", false},
};

const Setting* FindSetting(const base::String& key) {
  for (const Setting& setting : kSettings)
    if (key == setting.key)
      return &setting;
  return nullptr;
}

base::String Trim(const base::String& text) {
  size_t begin = 0, end = text.size();
  while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r'))
    ++begin;
  while (end > begin &&
         (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r'))
    --end;
  return base::String(text.c_str() + begin, end - begin);
}

// Splits "key rest of the line" into its first word and the remainder.
void SplitFirstWord(const base::String& line, base::String* key, base::String* rest) {
  size_t i = 0;
  while (i < line.size() && line[i] != ' ' && line[i] != '\t')
    ++i;
  *key = base::String(line.c_str(), i);
  *rest = Trim(base::String(line.c_str() + i, line.size() - i));
}

}  // namespace

ServerConfig ParseServerConfig(const base::String& text) {
  ServerConfig config;
  std::istringstream stream(std::string(text.c_str(), text.size()));
  std::string raw;
  // std::getline fills a std::string; each line is copied into a base::String.
  while (std::getline(stream, raw)) {
    const base::String line = Trim(base::String(raw.c_str(), raw.size()));
    if (line.empty() || line[0] == '#')
      continue;

    base::String key, rest;
    SplitFirstWord(line, &key, &rest);

    if (const Setting* setting = FindSetting(key)) {
      if (setting->takes_value && rest.empty()) {
        config.errors.push_back(base::String("'") + key + "' needs a value");
        continue;
      }
      config.args.push_back(base::String(setting->flag));
      if (setting->takes_value)
        config.args.push_back(rest);
      continue;
    }

    if (key == "set") {
      base::String convar, value;
      SplitFirstWord(rest, &convar, &value);
      if (convar.empty() || value.empty()) {
        config.errors.push_back(base::String("'set' needs a convar and a value"));
        continue;
      }
      config.convars.push_back({convar, value});
      continue;
    }

    // Not a setting: the console knows what to do with it, or will say it does
    // not. Kept verbatim so a command's own argument spacing survives.
    config.console_lines.push_back(line);
  }
  return config;
}

ServerConfig LoadServerConfig(const base::String& path, bool* out_found) {
  std::ifstream file(path.c_str(), std::ios::binary);
  if (out_found)
    *out_found = file.good();
  if (!file.good())
    return ServerConfig{};
  std::ostringstream text;
  text << file.rdbuf();
  const std::string contents = text.str();
  return ParseServerConfig(base::String(contents.c_str(), contents.size()));
}

}  // namespace rx
