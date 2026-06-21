#include <cstdlib>
#include <fstream>
#include <string>

#include "core/log.h"
#include "ecs/world.h"
#include "editor.h"
#include "editor_layout.h"
#include "engine_context.h"
#include "world/cell_streaming.h"

namespace rec {
namespace {

constexpr f32 kUnitsToMeters = 0.01428f;  // mirrors CellStreamer (unit -> metre)

// Where the layout lives: REC_EDITOR_LAYOUT, else a file in the working dir.
std::string DefaultLayoutPath() {
  if (const char* env = std::getenv("REC_EDITOR_LAYOUT")) return env;
  return "editor_layout.reclayout";
}

}  // namespace

int MapEditor::SaveLayout() {
  if (layout_path_.empty()) layout_path_ = DefaultLayoutPath();
  std::ofstream out(layout_path_, std::ios::trunc);
  if (!out) {
    SetStatus("Save failed: " + layout_path_);
    REC_WARN("editor: cannot open {} for writing", layout_path_);
    return 0;
  }
  out << "# recreation map layout v1\n";
  out << "# place <game> <plugin> <local_id> <px py pz> <qx qy qz qw> <scale>\n";
  int n = 0;
  for (const PlacedObject& p : placed_) {
    if (!ctx_.world->IsAlive(p.entity)) continue;
    const world::Transform* t = ctx_.world->Get<world::Transform>(p.entity);
    if (!t) continue;
    editor::LayoutEntry e;
    e.domain = (p.domain >= 0 && p.domain < static_cast<int>(domains_.size()))
                   ? domains_[p.domain].tag
                   : "primary";
    e.base = p.base;
    for (int i = 0; i < 3; ++i) e.pos[i] = t->position[i];
    for (int i = 0; i < 4; ++i) e.rot[i] = t->rotation[i];
    e.scale = t->scale / kUnitsToMeters;  // native multiplier, not engine metres
    out << editor::FormatPlaceLine(e) << '\n';
    ++n;
  }
  SetStatus("Saved " + std::to_string(n) + " objects to " + layout_path_);
  REC_INFO("editor: saved {} objects to {}", n, layout_path_);
  return n;
}

int MapEditor::LoadLayout() {
  if (layout_path_.empty()) layout_path_ = DefaultLayoutPath();
  std::ifstream in(layout_path_);
  if (!in) return 0;  // no layout saved yet: a silent no-op
  if (!ctx_.world) return 0;
  EnsureDomains();

  std::string line;
  int n = 0, skipped = 0;
  editor::LayoutEntry le;
  while (std::getline(in, line)) {
    if (!editor::ParsePlaceLine(line, &le)) continue;
    // Map the saved game slug back to a loaded domain; a placement whose game is
    // not loaded this run is skipped rather than placed against the wrong assets.
    int domain = -1;
    for (int d = 0; d < static_cast<int>(domains_.size()); ++d) {
      if (domains_[d].tag == le.domain) {
        domain = d;
        break;
      }
    }
    if (domain < 0) {
      ++skipped;
      continue;
    }
    world::CellStreamer* streamer = StreamerFor(domain);
    if (!streamer) continue;
    ecs::Entity e = streamer->PlaceObject(*ctx_.world, le.base,
                                          Vec3{le.pos[0], le.pos[1], le.pos[2]}, le.rot, le.scale);
    if (e == ecs::kInvalidEntity) continue;
    // Recover a display name from the catalog when it is built.
    std::string name = "object";
    for (const CatalogEntry& c : catalog_) {
      if (c.domain == domain && c.base.plugin == le.base.plugin &&
          c.base.local_id == le.base.local_id) {
        name = c.name;
        break;
      }
    }
    placed_.push_back({e, le.base, std::move(name), domain});
    ++n;
  }
  if (skipped > 0) REC_INFO("editor: skipped {} placements for unloaded games", skipped);
  if (n > 0) {
    SetStatus("Loaded " + std::to_string(n) + " saved objects");
    REC_INFO("editor: loaded {} objects from {}", n, layout_path_);
  }
  return n;
}

}  // namespace rec
