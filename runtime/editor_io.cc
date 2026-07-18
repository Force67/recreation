#include <base/option.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "core/log.h"
#include "ecs/world.h"
#include "editor.h"
#include "editor_layout.h"
#include "engine_context.h"
#include "world/cell_streaming.h"

namespace rx {
namespace {

constexpr f32 kUnitsToMeters = 0.01428f;  // mirrors CellStreamer (unit -> metre)

// Config override, populated from the environment by
// base::InitOptionsFromEnv() at startup.
base::Option<const char*> EditorLayout{"editor.layout", nullptr, "RX_EDITOR_LAYOUT"};
base::Option<const char*> TerrainEditsPath{"terrain.edits", nullptr, "REC_TERRAIN_EDITS"};

// Where the layout lives: RX_EDITOR_LAYOUT, else a file in the working dir.
std::string DefaultLayoutPath() {
  if (const char* env = EditorLayout.get()) return env;
  return "editor_layout.reclayout";
}

std::string DefaultTerrainPath(const std::string& layout_path, std::string_view world_identity) {
  if (const char* env = TerrainEditsPath.get()) return env;
  std::filesystem::path path(layout_path);
  std::string slug;
  for (char c : world_identity) {
    const unsigned char byte = static_cast<unsigned char>(c);
    const char safe = std::isalnum(byte) ? static_cast<char>(std::tolower(byte)) : '_';
    if (safe != '_' || slug.empty() || slug.back() != '_') slug += safe;
  }
  while (!slug.empty() && slug.back() == '_') slug.pop_back();
  if (slug.empty()) slug = "world";
  u64 identity_hash = 0xcbf29ce484222325ull;
  for (unsigned char byte : world_identity) {
    identity_hash ^= byte;
    identity_hash *= 0x100000001b3ull;
  }
  slug += "_" + std::to_string(identity_hash);
  path.replace_extension("." + slug + ".recterrain");
  return path.string();
}

}  // namespace

std::optional<int> MapEditor::SaveLayout() {
  if (layout_path_.empty()) layout_path_ = DefaultLayoutPath();
  std::ofstream out(layout_path_, std::ios::trunc);
  if (!out) {
    SetStatus("Save failed: " + layout_path_);
    RX_WARN("editor: cannot open {} for writing", layout_path_);
    return std::nullopt;
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
  out.flush();
  if (!out) {
    SetStatus("Save failed: " + layout_path_);
    RX_WARN("editor: failed while writing {}", layout_path_);
    return std::nullopt;
  }
  SetStatus("Saved " + std::to_string(n) + " objects to " + layout_path_);
  RX_INFO("editor: saved {} objects to {}", n, layout_path_);
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
  if (skipped > 0) RX_INFO("editor: skipped {} placements for unloaded games", skipped);
  if (n > 0) {
    SetStatus("Loaded " + std::to_string(n) + " saved objects");
    RX_INFO("editor: loaded {} objects from {}", n, layout_path_);
  }
  return n;
}

bool MapEditor::SaveTerrain() {
  if (!ctx_.streamer) return false;
  if (terrain_load_failed_) {
    SetStatus("Terrain save blocked: fix or remove the rejected diff first");
    RX_WARN("editor: preserving rejected terrain diff {}", terrain_path_);
    return false;
  }
  if (layout_path_.empty()) layout_path_ = DefaultLayoutPath();
  if (!TerrainEditsPath.get() || terrain_path_.empty()) {
    terrain_path_ = DefaultTerrainPath(layout_path_, ctx_.streamer->terrain_world_identity());
  }
  std::string error;
  if (!ctx_.streamer->SaveTerrainEdits(terrain_path_, &error)) {
    SetStatus("Terrain save failed: " + error);
    RX_WARN("editor: cannot save terrain diff {}: {}", terrain_path_, error);
    return false;
  }
  RX_INFO("editor: saved {} terrain samples to {}", ctx_.streamer->terrain_edit_sample_count(),
          terrain_path_);
  return true;
}

bool MapEditor::LoadTerrain() {
  if (!ctx_.streamer || !ctx_.world) return false;
  if (layout_path_.empty()) layout_path_ = DefaultLayoutPath();
  if (!TerrainEditsPath.get() || terrain_path_.empty()) {
    terrain_path_ = DefaultTerrainPath(layout_path_, ctx_.streamer->terrain_world_identity());
  }
  if (!std::filesystem::exists(terrain_path_)) {
    terrain_load_failed_ = false;
    return true;
  }
  std::string error;
  if (!ctx_.streamer->LoadTerrainEdits(*ctx_.world, terrain_path_, &error)) {
    terrain_load_failed_ = true;
    SetStatus("Terrain load rejected: " + error);
    RX_WARN("editor: rejected terrain diff {}: {}", terrain_path_, error);
    return false;
  }
  terrain_load_failed_ = false;
  RX_INFO("editor: loaded {} terrain samples from {}", ctx_.streamer->terrain_edit_sample_count(),
          terrain_path_);
  return true;
}

void MapEditor::SaveEditorData() {
  FinishTerrainStroke();
  const std::optional<int> objects = SaveLayout();
  const bool terrain_saved = SaveTerrain();
  if (objects && terrain_saved) {
    SetStatus("Saved " + std::to_string(*objects) + " objects and " +
              std::to_string(ctx_.streamer->terrain_edit_sample_count()) + " terrain samples");
  }
}

}  // namespace rx
