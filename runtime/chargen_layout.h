#ifndef RECREATION_RUNTIME_CHARGEN_LAYOUT_H_
#define RECREATION_RUNTIME_CHARGEN_LAYOUT_H_

#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/types.h"

namespace rx::chargen {

// A saved character-creation preset: the race blend, sex, chosen RPRM preset and
// hairstyle, the skin + hair colours, the four NAMA face-part type indices, the
// sparse set of non-zero NAM9 sliders and any layered chargen-tri morph
// overrides. The file is a tiny line-based key/value format so it stays diff
// friendly and dependency-free; the format and its parser live here, header-only,
// so the ctest gate can round-trip them without the engine (mirrors
// runtime/editor_layout.h).
struct CharGenPreset {
  std::string race = "NordRace";  // race-blend morph name (RACE EDID)
  int sex = 0;                    // 0 male, 1 female
  int preset = 0;                 // index into the race/sex RPRM preset list
  int subdiv = 1;                 // loop-subdivision levels on the head parts
  int hair_style = -1;            // index into the race/sex hair HDPT list (-1 keep)
  int hair_color = -1;            // index into the race/sex AHCM list (-1 keep)
  f32 skin[3] = {0.6f, 0.5f, 0.45f};
  f32 hair[3] = {0.32f, 0.24f, 0.18f};
  int nama[4] = {-1, -1, -1, -1};             // nose, brows, eyes, mouth
  std::vector<std::pair<int, f32>> nam9;      // sparse: (slider index, value)
  std::vector<std::pair<std::string, f32>> morphs;  // layered chargen morphs
};

inline std::string SerializeCharGenPreset(const CharGenPreset& p) {
  std::ostringstream out;
  out << "# recreation chargen preset v1\n";
  out << "race " << (p.race.empty() ? "NordRace" : p.race) << '\n';
  out << "sex " << p.sex << '\n';
  out << "preset " << p.preset << '\n';
  out << "subdiv " << p.subdiv << '\n';
  out << "hairstyle " << p.hair_style << '\n';
  out << "haircolor " << p.hair_color << '\n';
  out << "skin " << p.skin[0] << ' ' << p.skin[1] << ' ' << p.skin[2] << '\n';
  out << "hair " << p.hair[0] << ' ' << p.hair[1] << ' ' << p.hair[2] << '\n';
  out << "nama " << p.nama[0] << ' ' << p.nama[1] << ' ' << p.nama[2] << ' ' << p.nama[3] << '\n';
  for (const auto& [i, v] : p.nam9) out << "nam9 " << i << ' ' << v << '\n';
  for (const auto& [name, w] : p.morphs) out << "morph " << name << ' ' << w << '\n';
  return out.str();
}

// Parses a whole preset document into `out`. Unknown lines, comments (leading
// '#') and blanks are skipped. Returns true when at least one recognized field
// was read, so a truncated or unrelated file reports failure and the caller keeps
// its defaults.
inline bool ParseCharGenPreset(const std::string& text, CharGenPreset* out) {
  CharGenPreset p;
  bool any = false;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ss(line);
    std::string tag;
    ss >> tag;
    if (tag == "race") {
      ss >> p.race;
      any = true;
    } else if (tag == "sex") {
      ss >> p.sex;
      any = true;
    } else if (tag == "preset") {
      ss >> p.preset;
      any = true;
    } else if (tag == "subdiv") {
      ss >> p.subdiv;
      any = true;
    } else if (tag == "hairstyle") {
      ss >> p.hair_style;
      any = true;
    } else if (tag == "haircolor") {
      ss >> p.hair_color;
      any = true;
    } else if (tag == "skin") {
      ss >> p.skin[0] >> p.skin[1] >> p.skin[2];
      any = true;
    } else if (tag == "hair") {
      ss >> p.hair[0] >> p.hair[1] >> p.hair[2];
      any = true;
    } else if (tag == "nama") {
      ss >> p.nama[0] >> p.nama[1] >> p.nama[2] >> p.nama[3];
      any = true;
    } else if (tag == "nam9") {
      int i = -1;
      f32 v = 0;
      if (ss >> i >> v && i >= 0) {
        p.nam9.emplace_back(i, v);
        any = true;
      }
    } else if (tag == "morph") {
      std::string name;
      f32 w = 0;
      if (ss >> name >> w) {
        p.morphs.emplace_back(std::move(name), w);
        any = true;
      }
    }
  }
  if (any) *out = std::move(p);
  return any;
}

}  // namespace rx::chargen

#endif  // RECREATION_RUNTIME_CHARGEN_LAYOUT_H_
