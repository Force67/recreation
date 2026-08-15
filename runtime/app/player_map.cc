#include <cmath>
#include <cstdio>
#include <cstring>

#include <base/algorithm.h>
#include <base/option.h>

#include "components/bethesda/record.h"
#include "components/world/cell_streaming.h"
#include "components/world/map_discovery.h"
#include "components/world/map_markers.h"
#include "core/input.h"
#include "core/log.h"
#include "runtime/actor/actor_system.h"
#include "runtime/app/engine.h"
#include "runtime/app/engine_internal.h"

// The world map screen and fast travel.
//
// The map picture is painted here rather than assembled out of widgets: the fog
// of war is a quarter of a million cell tiles and ugui has no canvas, so the
// whole thing goes into one RGBA texture that the UI shows as a single image.
// Everything drawn comes from the two stores (MapDiscovery for what has been
// uncovered, MapMarkers for the named places); this file owns no state about the
// world beyond where the view is looking.
namespace rx {

// Test hooks, so a scripted run can show the map and prove a fast travel without
// a hand on the keyboard. Namespace scope, like every other Option here, so they
// register before the environment is read.
static base::Option<bool> MapOpen{"map.open", false, "RX_PLAYER_MAP",
                                  "open the world map as soon as the world is up"};
static base::Option<const char*> MapTravel{"map.travel", nullptr, "RX_FAST_TRAVEL",
                                           "fast travel to this location once, at boot"};

namespace {

constexpr u32 kEdid = FourCc('E', 'D', 'I', 'D');
constexpr u32 kFull = FourCc('F', 'U', 'L', 'L');

// Palette, on the same monochrome system as the rest of the UI: the map is
// values, never colour.
constexpr u32 kMapBackground = 0x0a0a0aff;
constexpr u32 kMapKnown = 0x3c3c3cff;      // an uncovered map tile
constexpr u32 kMapGrid = 0x1a1a1aff;       // cell boundaries inside the known area
constexpr u32 kMapMarkerTravel = 0xffffffff;
constexpr u32 kMapMarkerSeen = 0x8a8a8aff;  // visible, but not a travel destination
constexpr u32 kMapPlayer = 0xffffffff;

// One exterior cell is 16 map tiles on a side (MapDiscovery::kCellSubdivisions).
constexpr f32 kCellUnits = 4096.0f;

// How far the framed view can be zoomed either way.
constexpr f32 kMinZoom = 0.5f;
constexpr f32 kMaxZoom = 8.0f;

// The pace the player would have walked it: MOVT NPC_Default_MT's forward walk
// speed (see player_controller.cc). Fast travel spends the game time the walk
// would have taken, which is the real seconds at that speed times the clock's
// own timescale.
constexpr f32 kWalkUnitsPerSecond = 80.10f;

struct Painter {
  u8* pixels;
  int width;
  int height;

  void Plot(int x, int y, u32 rgba) {
    if (x < 0 || y < 0 || x >= width || y >= height)
      return;
    u8* p = pixels + (static_cast<mem_size>(y) * width + x) * 4;
    p[0] = static_cast<u8>(rgba >> 24);
    p[1] = static_cast<u8>(rgba >> 16);
    p[2] = static_cast<u8>(rgba >> 8);
    p[3] = static_cast<u8>(rgba);
  }

  void Fill(int x0, int y0, int x1, int y1, u32 rgba) {
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x)
        Plot(x, y, rgba);
  }

  void Box(int cx, int cy, int radius, u32 rgba) {
    for (int d = -radius; d <= radius; ++d) {
      Plot(cx + d, cy - radius, rgba);
      Plot(cx + d, cy + radius, rgba);
      Plot(cx - radius, cy + d, rgba);
      Plot(cx + radius, cy + d, rgba);
    }
  }
};

// Game units to canvas pixels. Game +y is north, which is up on the canvas, so
// the y axis flips.
struct MapView {
  f32 centre_x = 0, centre_y = 0;
  f32 scale = 1.0f;  // canvas pixels per game unit
  f32 pan_x = 0, pan_y = 0;
  int width = 0, height = 0;

  f32 ToCanvasX(f32 game_x) const {
    return (game_x - centre_x) * scale + static_cast<f32>(width) * 0.5f + pan_x;
  }
  f32 ToCanvasY(f32 game_y) const {
    return static_cast<f32>(height) * 0.5f - (game_y - centre_y) * scale + pan_y;
  }
};

base::String RecordText(const bethesda::RecordStore& records,
                        const bethesda::StringTable& strings,
                        bethesda::GlobalFormId id) {
  const bethesda::RecordStore::StoredRecord* stored = records.Find(id);
  bethesda::Record record;
  if (!stored || !records.Parse(id, &record))
    return {};
  if (const bethesda::Subrecord* full = record.Find(kFull); full && full->data.size() >= 4) {
    u32 string_id;
    std::memcpy(&string_id, full->data.data(), 4);
    if (const base::String* text = strings.Find(string_id, stored->winning_plugin))
      return *text;
  }
  return record.GetString(kEdid);
}

// Engine space (Y up, metres) back to game units (Z up), the inverse of the
// conversion every streamed reference goes through.
void ToGameUnits(const Vec3& position, f32 units_to_meters, f32 out[3]) {
  if (units_to_meters <= 0.0f) {
    out[0] = out[1] = out[2] = 0.0f;
    return;
  }
  out[0] = position.x / units_to_meters;
  out[1] = -position.z / units_to_meters;
  out[2] = position.y / units_to_meters;
}

base::String FormatDistance(f32 units) {
  // Game units to metres, the same conversion every streamed reference goes
  // through.
  const f32 metres = units * 0.01428f;
  char text[32];
  if (metres >= 1000.0f)
    std::snprintf(text, sizeof(text), "%.1f km", metres / 1000.0f);
  else
    std::snprintf(text, sizeof(text), "%.0f m", metres);
  return text;
}

}  // namespace

void TogglePlayerMap(Engine& engine) {
  Engine* const self = &engine;
  self->player_map_.open = !self->player_map_.open;
  self->player_map_.dirty = true;
  if (self->player_map_.open) {
    // Reopening reframes on the player rather than resuming wherever the last
    // pan left the view.
    self->player_map_.pan_x = 0.0f;
    self->player_map_.pan_y = 0.0f;
    self->player_map_.zoom = 1.0f;
    self->player_map_.selected = 0;
    self->player_map_.status.clear();
  }
}

void UpdatePlayerMapInput(Engine& engine, const InputState& input, const ActionState& actions) {
  Engine* const self = &engine;
  PlayerMapState& map = self->player_map_;
  const int count = static_cast<int>(map.shown.size());

  int move = 0;
  if (input.key_pressed(Key::kW))
    --move;
  if (input.key_pressed(Key::kS))
    ++move;
  if (move != 0 && count > 0) {
    map.selected = base::Clamp(map.selected + move, 0, count - 1);
    // Picking a place looks at it: the view rides the selection until the
    // player pans away from it by hand.
    map.follow_selection = true;
    map.pan_x = 0.0f;
    map.pan_y = 0.0f;
    map.dirty = true;
  }

  // Arrows pan, and panning takes the view off the selection.
  constexpr f32 kPanStep = 60.0f;
  const bool pan_left = input.key_pressed(Key::kArrowLeft);
  const bool pan_right = input.key_pressed(Key::kArrowRight);
  const bool pan_up = input.key_pressed(Key::kArrowUp);
  const bool pan_down = input.key_pressed(Key::kArrowDown);
  if (pan_left || pan_right || pan_up || pan_down) {
    map.pan_x += (pan_left ? kPanStep : 0.0f) - (pan_right ? kPanStep : 0.0f);
    map.pan_y += (pan_up ? kPanStep : 0.0f) - (pan_down ? kPanStep : 0.0f);
    map.follow_selection = false;
    map.dirty = true;
  }

  if (input.key_pressed(Key::kE)) {
    map.zoom = base::Min(map.zoom * 1.5f, kMaxZoom);
    map.dirty = true;
  }
  if (input.key_pressed(Key::kQ)) {
    map.zoom = base::Max(map.zoom / 1.5f, kMinZoom);
    map.dirty = true;
  }

  if (input.key_pressed(Key::kReturn) && map.selected >= 0 && map.selected < count)
    FastTravelToMarker(engine, self->map_markers_.all()[map.shown[map.selected]].ref);

  if (actions.pressed(Action::kMenuCancel) || input.key_pressed(Key::kM))
    TogglePlayerMap(engine);
}

bool FastTravelToMarker(Engine& engine, bethesda::GlobalFormId marker_ref) {
  Engine* const self = &engine;
  const world::MapMarker* marker = self->map_markers_.Find(marker_ref);
  if (!marker || !self->streamer_)
    return false;
  if (!marker->can_travel) {
    self->player_map_.status = marker->name + " cannot be travelled to";
    return false;
  }

  // Where the player is now, so the trip has a length. The camera stands in for
  // a player the fly camera has not spawned.
  Vec3 standing = self->camera_.position();
  if (self->actors_)
    self->actors_->PlayerWorldPos(&standing);
  f32 from[3];
  ToGameUnits(standing, bethesda::GameProfile::For(self->game_).units_to_meters, from);
  const f32 dx = marker->position[0] - from[0];
  const f32 dy = marker->position[1] - from[1];
  const f32 distance = std::sqrt(dx * dx + dy * dy);

  // Leaving where the player is: an interior has to be dropped, and a marker in
  // another worldspace means restreaming that world from scratch.
  if (self->streamer_->in_interior())
    self->streamer_->EnterExterior(*self->world_);
  if (marker->worldspace.plugin != 0xffff &&
      marker->worldspace.packed() != self->streamer_->worldspace().packed()) {
    // SelectWorldspace takes the editor id, which is what the WRLD record is
    // found by; the display name is a different string.
    bethesda::Record record;
    base::String world_edid;
    if (self->records_.Parse(marker->worldspace, &record))
      world_edid = record.GetString(kEdid);
    self->streamer_->UnloadAllCells(*self->world_);
    if (!self->streamer_->SelectWorldspace(world_edid)) {
      self->player_map_.status = "Cannot reach " + marker->name;
      RX_WARN("map: fast travel to {} needs worldspace {}, which did not select", marker->name,
              world_edid);
      return false;
    }
    RX_INFO("map: fast travel crosses into worldspace {}", world_edid);
  }

  const f32 scale = bethesda::GameProfile::For(self->game_).units_to_meters;
  // Game units back to engine space, the same axis change
  // the savegame loader places the player with. The marker sits on the ground,
  // so the player lands on it.
  Vec3 feet{marker->position[0] * scale, marker->position[2] * scale,
            -marker->position[1] * scale};
  // The marker sits on the ground the game authored, which is not to the
  // millimetre the ground this engine bakes, and a capsule that lands inside the
  // heightfield leaves through the floor (see CellStreamer::kGroundClearance).
  if (f32 ground = 0; !self->streamer_->in_interior() &&
                      self->streamer_->GroundHeight(feet.x, feet.z, &ground) &&
                      feet.y < ground + world::CellStreamer::kGroundClearance) {
    feet.y = ground + world::CellStreamer::kGroundClearance;
  }
  self->actors_->TeleportPlayer(feet.x, feet.y, feet.z);
  self->camera_.set_position({feet.x, feet.y + 1.8f, feet.z});

  // The clock spends what the walk would have cost: real seconds at walking
  // pace, run through the world's timescale.
  f32 hours = 0.0f;
  if (self->clock_) {
    const f64 real_seconds = static_cast<f64>(distance) / kWalkUnitsPerSecond;
    const f64 game_seconds = real_seconds * static_cast<f64>(self->clock_->timescale());
    hours = static_cast<f32>(game_seconds / 3600.0);
    self->clock_->set_game_days(self->clock_->game_days() + game_seconds / 86400.0);
  }

  char status[160];
  std::snprintf(status, sizeof(status), "Travelled to %s  ·  %s  ·  %.1f h",
                marker->name.c_str(), FormatDistance(distance).c_str(), hours);
  self->player_map_.status = status;
  self->player_map_.dirty = true;
  RX_INFO("map: fast travel from game ({:.1f}, {:.1f}, {:.1f}) to {} at ({:.1f}, {:.1f}, {:.1f}) "
          "/ engine ({:.1f}, {:.1f}, {:.1f}), {} away, {:.1f} game hours spent",
          from[0], from[1], from[2], marker->name, marker->position[0], marker->position[1],
          marker->position[2], feet.x, feet.y, feet.z, FormatDistance(distance), hours);
  return true;
}

void RefreshPlayerMap(Engine& engine, f32 dt) {
  Engine* const self = &engine;
  PlayerMapState& map = self->player_map_;
  if (!map.boot_applied && self->streamer_ && !self->map_markers_.all().empty()) {
    map.boot_applied = true;
    if (MapOpen.get())
      TogglePlayerMap(engine);
    if (const char* want = MapTravel.get()) {
      const world::MapMarker* found = nullptr;
      for (const world::MapMarker& marker : self->map_markers_.all()) {
        if (marker.visible && marker.name == want) {
          found = &marker;
          break;
        }
      }
      if (found)
        FastTravelToMarker(engine, found->ref);
      else
        RX_WARN("map: no discovered location called '{}' to travel to", want);
    }
  }
  if (!map.open) {
    if (map.dirty) {
      self->game_ui_.SetPlayerMap({});
      map.dirty = false;
    }
    return;
  }
  (void)dt;

  const bethesda::GlobalFormId here =
      self->streamer_ ? self->streamer_->worldspace() : bethesda::GlobalFormId{};
  const bethesda::GlobalFormId shown = MapWorldspaceFor(self->records_, here);

  Vec3 standing = self->camera_.position();
  if (self->actors_)
    self->actors_->PlayerWorldPos(&standing);
  f32 player[3];
  ToGameUnits(standing, bethesda::GameProfile::For(self->game_).units_to_meters, player);

  // The list: every discovered location on this map, nearest first, so the
  // selection starts on somewhere the player can recognise.
  const base::Vector<world::MapMarker>& all = self->map_markers_.all();
  map.shown.clear();
  for (u32 i = 0; i < all.size(); ++i) {
    if (!all[i].visible)
      continue;
    if (shown.plugin != 0xffff && all[i].map_worldspace.packed() != shown.packed())
      continue;
    map.shown.push_back(i);
  }
  base::Sort(map.shown.begin(), map.shown.end(), [&](u32 a, u32 b) {
    const auto distance = [&](const world::MapMarker& m) {
      const f32 dx = m.position[0] - player[0];
      const f32 dy = m.position[1] - player[1];
      return dx * dx + dy * dy;
    };
    return distance(all[a]) < distance(all[b]);
  });
  if (map.shown.empty())
    map.selected = 0;
  else
    map.selected = base::Clamp(map.selected, 0, static_cast<int>(map.shown.size()) - 1);

  // Frame: everything known on this map, plus every marker on it.
  bool have_bounds = false;
  f32 min_x = 0, min_y = 0, max_x = 0, max_y = 0;
  const auto include = [&](f32 x, f32 y) {
    if (!have_bounds) {
      min_x = max_x = x;
      min_y = max_y = y;
      have_bounds = true;
      return;
    }
    min_x = base::Min(min_x, x);
    min_y = base::Min(min_y, y);
    max_x = base::Max(max_x, x);
    max_y = base::Max(max_y, y);
  };
  // A city worldspace's cells are uncovered under their own id but sit on the
  // parent's map at the same coordinates, so both contribute.
  base::Vector<bethesda::GlobalFormId> worlds;
  for (bethesda::GlobalFormId world : self->map_discovery_.Worldspaces()) {
    if (MapWorldspaceFor(self->records_, world).packed() != shown.packed())
      continue;
    worlds.push_back(world);
    i16 lo_x, lo_y, hi_x, hi_y;
    if (!self->map_discovery_.Bounds(world, &lo_x, &lo_y, &hi_x, &hi_y))
      continue;
    include(static_cast<f32>(lo_x) * kCellUnits, static_cast<f32>(lo_y) * kCellUnits);
    include(static_cast<f32>(hi_x + 1) * kCellUnits, static_cast<f32>(hi_y + 1) * kCellUnits);
  }
  for (u32 index : map.shown)
    include(all[index].position[0], all[index].position[1]);
  if (!have_bounds) {
    min_x = player[0] - kCellUnits;
    max_x = player[0] + kCellUnits;
    min_y = player[1] - kCellUnits;
    max_y = player[1] + kCellUnits;
  }

  MapView view;
  view.width = PlayerMapState::kCanvasWidth;
  view.height = PlayerMapState::kCanvasHeight;
  view.centre_x = (min_x + max_x) * 0.5f;
  view.centre_y = (min_y + max_y) * 0.5f;
  // Framed to fit, the whole map is on screen and there is nothing to follow;
  // zoomed in, the view rides the selection so picking a place looks at it.
  if (map.follow_selection && map.zoom > 1.01f && !map.shown.empty()) {
    const world::MapMarker& on = all[map.shown[map.selected]];
    view.centre_x = on.position[0];
    view.centre_y = on.position[1];
  }
  const f32 span_x = base::Max(max_x - min_x, 1.0f);
  const f32 span_y = base::Max(max_y - min_y, 1.0f);
  const f32 fit = base::Min(static_cast<f32>(view.width - 24) / span_x,
                            static_cast<f32>(view.height - 24) / span_y);
  view.scale = fit * map.zoom;
  view.pan_x = map.pan_x;
  view.pan_y = map.pan_y;

  const mem_size bytes = static_cast<mem_size>(view.width) * view.height * 4;
  if (map.pixels.size() != bytes)
    map.pixels.resize(bytes);
  Painter painter{map.pixels.data(), view.width, view.height};
  painter.Fill(0, 0, view.width, view.height, kMapBackground);

  // Fog of war: one filled rectangle per uncovered sixteenth of a cell. The
  // 256 bits are read as 16 rows of 16, low bit first, with row 0 at the cell's
  // south edge; the save carries no statement of that order, so only the shape
  // of a partly-explored cell depends on it, never which cell is lit.
  const f32 tile = kCellUnits / world::MapDiscovery::kCellSubdivisions;
  for (bethesda::GlobalFormId world : worlds) {
    i16 lo_x, lo_y, hi_x, hi_y;
    if (!self->map_discovery_.Bounds(world, &lo_x, &lo_y, &hi_x, &hi_y))
      continue;
    for (i32 cy = lo_y; cy <= hi_y; ++cy) {
      for (i32 cx = lo_x; cx <= hi_x; ++cx) {
        const u32 tiles = self->map_discovery_.CellTiles(world, static_cast<i16>(cx),
                                                         static_cast<i16>(cy));
        if (tiles == 0)
          continue;
        const f32 base_x = static_cast<f32>(cx) * kCellUnits;
        const f32 base_y = static_cast<f32>(cy) * kCellUnits;
        // A whole cell is one rectangle once it is smaller than a few pixels,
        // which is most of the map at the default zoom.
        if (view.scale * kCellUnits < 8.0f) {
          const int x0 = static_cast<int>(view.ToCanvasX(base_x));
          const int y0 = static_cast<int>(view.ToCanvasY(base_y + kCellUnits));
          const int x1 = base::Max(x0 + 1, static_cast<int>(view.ToCanvasX(base_x + kCellUnits)));
          const int y1 = base::Max(y0 + 1, static_cast<int>(view.ToCanvasY(base_y)));
          painter.Fill(x0, y0, x1, y1, kMapKnown);
          continue;
        }
        for (u32 row = 0; row < world::MapDiscovery::kCellSubdivisions; ++row) {
          for (u32 col = 0; col < world::MapDiscovery::kCellSubdivisions; ++col) {
            if (!self->map_discovery_.TileVisited(world, static_cast<i16>(cx),
                                                  static_cast<i16>(cy), col, row))
              continue;
            const f32 tx = base_x + static_cast<f32>(col) * tile;
            const f32 ty = base_y + static_cast<f32>(row) * tile;
            const int x0 = static_cast<int>(view.ToCanvasX(tx));
            const int y0 = static_cast<int>(view.ToCanvasY(ty + tile));
            const int x1 = base::Max(x0 + 1, static_cast<int>(view.ToCanvasX(tx + tile)));
            const int y1 = base::Max(y0 + 1, static_cast<int>(view.ToCanvasY(ty)));
            painter.Fill(x0, y0, x1, y1, kMapKnown);
          }
        }
        // A cell edge, so the grid the world is actually made of reads.
        const int gx = static_cast<int>(view.ToCanvasX(base_x));
        const int gy = static_cast<int>(view.ToCanvasY(base_y + kCellUnits));
        const int gx1 = static_cast<int>(view.ToCanvasX(base_x + kCellUnits));
        const int gy1 = static_cast<int>(view.ToCanvasY(base_y));
        painter.Fill(gx, gy, gx + 1, gy1, kMapGrid);
        painter.Fill(gx, gy, gx1, gy + 1, kMapGrid);
      }
    }
  }

  // Markers on top of the fog, the selected one ringed.
  for (u32 slot = 0; slot < map.shown.size(); ++slot) {
    const world::MapMarker& marker = all[map.shown[slot]];
    const int x = static_cast<int>(view.ToCanvasX(marker.position[0]));
    const int y = static_cast<int>(view.ToCanvasY(marker.position[1]));
    const u32 colour = marker.can_travel ? kMapMarkerTravel : kMapMarkerSeen;
    painter.Fill(x - 1, y - 1, x + 2, y + 2, colour);
    if (static_cast<int>(slot) == map.selected)
      painter.Box(x, y, 6, kMapMarkerTravel);
  }

  // The player: a small cross, which no marker draws.
  {
    const int x = static_cast<int>(view.ToCanvasX(player[0]));
    const int y = static_cast<int>(view.ToCanvasY(player[1]));
    painter.Fill(x - 5, y, x + 6, y + 1, kMapPlayer);
    painter.Fill(x, y - 5, x + 1, y + 6, kMapPlayer);
  }

  if (map.texture == 0) {
    map.texture = self->game_ui_.CreateUiTexture(view.width, view.height, map.pixels.data());
  } else {
    self->game_ui_.UpdateUiTexture(map.texture, map.pixels.data());
  }

  GameUi::PlayerMapView out;
  out.open = true;
  out.canvas = map.texture;
  out.title = RecordText(self->records_, self->strings_, shown);
  if (out.title.empty())
    out.title = "Map";
  char subtitle[96];
  std::snprintf(subtitle, sizeof(subtitle), "%u of %u locations found, %u cleared",
                self->map_markers_.VisibleCount(),
                static_cast<u32>(self->map_markers_.all().size()),
                self->map_markers_.ClearedCount());
  out.subtitle = subtitle;
  out.status = map.status;
  out.where = self->streamer_ && self->streamer_->in_interior()
                  ? RecordText(self->records_, self->strings_, self->streamer_->interior_cell())
                  : base::String();
  if (out.where.empty()) {
    char cell[64];
    std::snprintf(cell, sizeof(cell), "Cell %d, %d",
                  self->streamer_ ? self->streamer_->anchor_cell_x() : 0,
                  self->streamer_ ? self->streamer_->anchor_cell_y() : 0);
    out.where = cell;
  }

  // The rail shows a window around the selection, so a list of hundreds still
  // scrolls with the cursor.
  const int rows = GameUi::PlayerMapView::kRows;
  const int count = static_cast<int>(map.shown.size());
  int first = base::Clamp(map.selected - rows / 2, 0, base::Max(0, count - rows));
  for (int i = 0; i < rows && first + i < count; ++i) {
    const world::MapMarker& marker = all[map.shown[first + i]];
    GameUi::PlayerMapView::Row row;
    row.name = marker.name;
    row.travelable = marker.can_travel;
    const f32 dx = marker.position[0] - player[0];
    const f32 dy = marker.position[1] - player[1];
    row.detail = marker.can_travel ? FormatDistance(std::sqrt(dx * dx + dy * dy))
                                   : base::String("no route");
    // The same word the games put on a finished dungeon, beside the distance so
    // the rail reads as one line rather than two columns.
    if (marker.cleared)
      row.detail += "  cleared";
    out.rows.push_back(row);
  }
  out.selected = map.selected - first;
  self->game_ui_.SetPlayerMap(out);
  map.dirty = false;
}

}  // namespace rx
