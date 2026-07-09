#ifndef RECREATION_RUNTIME_EDITOR_LAYOUT_H_
#define RECREATION_RUNTIME_EDITOR_LAYOUT_H_

#include <sstream>
#include <string>

#include "bethesda/form_id.h"
#include "core/types.h"

namespace rx::editor {

// One placed object as stored in a layout file: the game it came from (a stable
// slug, e.g. "skyrimse" or "fallout4"), the base form it instantiates, and its
// engine-space transform (position, quaternion, native scale). The file is a
// tiny line-based format so it stays diff-friendly and dependency-free; the
// format and its parser live here, header-only, so the ctest gate can round-trip
// them without the engine.
struct LayoutEntry {
  std::string domain;  // game slug; never contains whitespace
  bethesda::GlobalFormId base;
  f32 pos[3] = {0, 0, 0};
  f32 rot[4] = {0, 0, 0, 1};
  f32 scale = 1.0f;
};

// "place <domain> <plugin> <local_id> <px py pz> <qx qy qz qw> <scale>".
inline std::string FormatPlaceLine(const LayoutEntry& e) {
  std::ostringstream out;
  out << "place " << (e.domain.empty() ? "primary" : e.domain) << ' ' << e.base.plugin << ' '
      << e.base.local_id << ' ' << e.pos[0] << ' ' << e.pos[1] << ' ' << e.pos[2] << ' ' << e.rot[0]
      << ' ' << e.rot[1] << ' ' << e.rot[2] << ' ' << e.rot[3] << ' ' << e.scale;
  return out.str();
}

// Parses one layout line into `out`. Returns false for blanks, comments (a line
// starting with '#') and any line that is not a well-formed "place" record, so
// the loader can skip them.
inline bool ParsePlaceLine(const std::string& line, LayoutEntry* out) {
  if (line.empty() || line[0] == '#') return false;
  std::istringstream ss(line);
  std::string tag;
  ss >> tag;
  if (tag != "place") return false;
  unsigned plugin = 0, local = 0;
  LayoutEntry e;
  if (!(ss >> e.domain >> plugin >> local >> e.pos[0] >> e.pos[1] >> e.pos[2] >> e.rot[0] >>
        e.rot[1] >> e.rot[2] >> e.rot[3] >> e.scale)) {
    return false;
  }
  e.base = bethesda::GlobalFormId{static_cast<u16>(plugin), static_cast<u32>(local)};
  *out = e;
  return true;
}

}  // namespace rx::editor

#endif  // RECREATION_RUNTIME_EDITOR_LAYOUT_H_
