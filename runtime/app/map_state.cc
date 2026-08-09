#include <cmath>
#include <cstring>

#include "components/bethesda/record.h"
#include "components/world/map_discovery.h"
#include "components/world/map_markers.h"
#include "core/log.h"
#include "runtime/actor/actor_system.h"
#include "runtime/app/engine.h"
#include "runtime/app/engine_internal.h"

// Map discovery on the live world: standing somewhere uncovers it, walking up to
// a named place puts it on the map, and the F5 window shows what the store
// holds. A resumed savegame fills the same stores (savegame_load.cc), so a
// loaded map and a walked one read alike.
namespace rx {
namespace {

constexpr u32 kEdid = FourCc('E', 'D', 'I', 'D');
constexpr u32 kFull = FourCc('F', 'U', 'L', 'L');
constexpr u32 kData = FourCc('D', 'A', 'T', 'A');
constexpr u32 kTnam = FourCc('T', 'N', 'A', 'M');
constexpr u32 kFnam = FourCc('F', 'N', 'A', 'M');
constexpr u32 kXmrk = FourCc('X', 'M', 'R', 'K');
constexpr u32 kWnam = FourCc('W', 'N', 'A', 'M');
constexpr u32 kPnam = FourCc('P', 'N', 'A', 'M');
constexpr u32 kRefr = FourCc('R', 'E', 'F', 'R');

// A reference the game keeps loaded whatever cell the player is in. Map markers
// are all placed this way, which is what makes the catalogue scan cheap: only
// these get parsed.
constexpr u32 kRecordFlagPersistent = 0x00000400;

// The map marker's own authored flags (REFR FNAM), the same two bits the save
// writes back (savegame_changeform.h). A hold capital is authored visible.
constexpr u8 kMarkerAuthoredVisible = 0x01;
constexpr u8 kMarkerAuthoredCanTravel = 0x02;

// WRLD PNAM: which of its parent's data a child worldspace borrows. A city
// worldspace that borrows the map data draws on its parent's map at the same
// coordinates, which is why Whiterun's marker shows up on Tamriel.
constexpr u16 kWorldUsesParentMap = 0x0004;

// How close the player has to walk to a location before it is discovered. The
// game uses a per-marker radius the records do not carry; one cell's width
// reads about right on foot and never fires from across a valley.
constexpr f32 kDiscoveryRadius = 3000.0f;

// Rebuilding the tile grid walks every visited cell, so it happens a few times a
// second while the window is open rather than every frame.
constexpr f32 kMapPanelInterval = 0.5f;

base::String RecordName(const bethesda::RecordStore& records,
                        const bethesda::StringTable& strings,
                        bethesda::GlobalFormId id) {
  bethesda::Record record;
  if (id.plugin == 0xffff || !records.Parse(id, &record))
    return {};
  const bethesda::Subrecord* full = record.Find(kFull);
  if (full && full->data.size() >= 4) {
    u32 string_id;
    std::memcpy(&string_id, full->data.data(), 4);
    if (const base::String* text = strings.Find(string_id))
      return *text;
  }
  return record.GetString(kEdid);
}

// A record's display name, resolved against the strings of the plugin that won
// the record. The flat table is keyed by string id alone and two plugins reuse
// the same ids, so an add-on's marker takes a Skyrim.esm string without this.
base::String MarkerName(const bethesda::StringTable& strings,
                        const bethesda::Record& record,
                        u16 plugin) {
  const bethesda::Subrecord* full = record.Find(kFull);
  if (full && full->data.size() >= 4) {
    u32 string_id;
    std::memcpy(&string_id, full->data.data(), 4);
    if (const base::String* text = strings.Find(string_id, plugin))
      return *text;
  }
  return record.GetString(kEdid);
}

}  // namespace

// Which map a worldspace draws on: itself, or the parent it borrows map data
// from. Skyrim's walled cities are their own worldspaces sharing Tamriel's
// coordinates, so their markers belong on Tamriel's map, not on nine tiny ones.
bethesda::GlobalFormId MapWorldspaceFor(const bethesda::RecordStore& records,
                                        bethesda::GlobalFormId worldspace) {
  // Bounded: a parent chain longer than this is a cycle, not a hierarchy.
  for (u32 depth = 0; depth < 8; ++depth) {
    const bethesda::RecordStore::StoredRecord* stored = records.Find(worldspace);
    if (!stored)
      return worldspace;
    bethesda::Record record;
    if (!records.Parse(worldspace, &record))
      return worldspace;
    const bethesda::Subrecord* wnam = record.Find(kWnam);
    const bethesda::Subrecord* pnam = record.Find(kPnam);
    if (!wnam || wnam->data.size() < 4 || !pnam || pnam->data.size() < 2)
      return worldspace;
    u16 flags;
    std::memcpy(&flags, pnam->data.data(), 2);
    if ((flags & kWorldUsesParentMap) == 0)
      return worldspace;
    u32 raw;
    std::memcpy(&raw, wnam->data.data(), 4);
    const bethesda::GlobalFormId parent =
        records.ResolveFrom(bethesda::RawFormId{raw}, stored->winning_plugin);
    if (parent.plugin == 0xffff || parent.packed() == worldspace.packed())
      return worldspace;
    worldspace = parent;
  }
  return worldspace;
}

void BuildMapMarkers(Engine& engine) {
  Engine* const self = &engine;
  self->map_markers_.Clear();
  u32 scanned = 0, authored_visible = 0;
  base::UnorderedMap<u64, u64> map_worlds;  // worldspace -> the map it draws on
  self->records_.EachOfType(
      kRefr, [&](bethesda::GlobalFormId id, const bethesda::RecordStore::StoredRecord& stored) {
        if ((stored.header.flags & kRecordFlagPersistent) == 0)
          return;
        bethesda::Record record;
        if (!self->records_.Parse(id, &record))
          return;
        // XMRK is the subrecord that makes a reference a map marker; it carries
        // no data of its own.
        if (!record.Find(kXmrk))
          return;
        const bethesda::Subrecord* data = record.Find(kData);
        if (!data || data->data.size() < 12)
          return;
        ++scanned;

        world::MapMarker marker;
        marker.ref = id;
        marker.worldspace = self->records_.WorldspaceOfRef(id);
        std::memcpy(marker.position, data->data.data(), 12);
        marker.name = MarkerName(self->strings_, record, stored.winning_plugin);
        if (const bethesda::Subrecord* tnam = record.Find(kTnam); tnam && !tnam->data.empty())
          marker.icon = tnam->data[0];
        if (const bethesda::Subrecord* fnam = record.Find(kFnam); fnam && !fnam->data.empty()) {
          marker.visible = (fnam->data[0] & kMarkerAuthoredVisible) != 0;
          marker.can_travel = (fnam->data[0] & kMarkerAuthoredCanTravel) != 0;
          authored_visible += marker.visible ? 1 : 0;
        }
        marker.map_worldspace = marker.worldspace;
        if (marker.worldspace.plugin != 0xffff) {
          u64* cached = map_worlds.find(marker.worldspace.packed());
          if (!cached) {
            const bethesda::GlobalFormId map = MapWorldspaceFor(self->records_, marker.worldspace);
            cached = map_worlds.emplace(marker.worldspace.packed(), map.packed()).first;
          }
          marker.map_worldspace =
              bethesda::GlobalFormId{static_cast<u16>(*cached >> 32), static_cast<u32>(*cached)};
        }
        self->map_markers_.Add(marker);
      });
  RX_INFO("map: {} markers in the records, {} of them visible from the start", scanned,
          authored_visible);
}

void MarkPlayerDiscovery(Engine& engine) {
  Engine* const self = &engine;
  if (!self->streamer_)
    return;
  // Only where the player is: the fly camera roams the whole worldspace and
  // uncovering the map by flying over it would make the store meaningless.
  if (!self->ctx_.walk_mode)
    return;
  if (self->streamer_->in_interior()) {
    const bethesda::GlobalFormId cell = self->streamer_->interior_cell();
    if (cell.plugin != 0xffff)
      self->map_discovery_.MarkInterior(cell);
    return;
  }
  const bethesda::GlobalFormId worldspace = self->streamer_->worldspace();
  if (worldspace.plugin == 0xffff)
    return;
  self->map_discovery_.MarkCell(worldspace, self->streamer_->anchor_cell_x(),
                                self->streamer_->anchor_cell_y());

  // Walking up to a place puts it on the map, which is the other half of what a
  // savegame's marker flags carry.
  Vec3 player;
  if (!self->actors_ || !self->actors_->PlayerWorldPos(&player))
    return;
  const f32 scale = bethesda::GameProfile::For(self->game_).units_to_meters;
  if (scale <= 0.0f)
    return;
  const f32 game[3] = {player.x / scale, -player.z / scale, player.y / scale};
  const world::MapMarker* found =
      self->map_markers_.NearestUndiscovered(worldspace, game, kDiscoveryRadius);
  if (!found)
    return;
  const base::String name = found->name;
  self->map_markers_.Discover(found->ref);
  RX_INFO("map: discovered {}", name);
}

void RefreshMapPanel(Engine& engine, f32 dt) {
  Engine* const self = &engine;
  MapPanel& panel = self->map_panel_;
  panel.available = self->streamer_ != nullptr;
  if (!panel.available || !self->debug_ui_.map_visible())
    return;
  self->map_panel_timer_ -= dt;
  if (self->map_panel_timer_ > 0.0f)
    return;
  self->map_panel_timer_ = kMapPanelInterval;

  const world::MapDiscovery& discovery = self->map_discovery_;
  const bethesda::GlobalFormId worldspace = self->streamer_->worldspace();
  panel.worldspace = RecordName(self->records_, self->strings_, worldspace);
  panel.visited_cells = discovery.VisitedCells(worldspace);
  panel.visited_interiors = discovery.VisitedInteriors();
  panel.player_outside = !self->streamer_->in_interior();
  panel.player_x = self->streamer_->anchor_cell_x();
  panel.player_y = self->streamer_->anchor_cell_y();
  panel.location = panel.player_outside
                       ? base::String()
                       : RecordName(self->records_, self->strings_,
                                    self->streamer_->interior_cell());

  panel.known_tiles = 0;
  panel.tiles.clear();
  if (!discovery.Bounds(worldspace, &panel.min_x, &panel.min_y, &panel.max_x, &panel.max_y))
    return;
  const int width = panel.max_x - panel.min_x + 1;
  const int height = panel.max_y - panel.min_y + 1;
  panel.tiles.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
  for (int row = 0; row < height; ++row) {
    for (int col = 0; col < width; ++col) {
      const u32 tiles = discovery.CellTiles(worldspace, static_cast<i16>(panel.min_x + col),
                                            static_cast<i16>(panel.min_y + row));
      panel.known_tiles += tiles;
      panel.tiles[static_cast<size_t>(row) * static_cast<size_t>(width) + static_cast<size_t>(col)] =
          static_cast<u8>(tiles > 255 ? 255 : tiles);
    }
  }
}

}  // namespace rx
