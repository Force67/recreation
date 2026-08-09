#include <cstring>

#include "components/bethesda/record.h"
#include "components/world/map_discovery.h"
#include "core/log.h"
#include "runtime/app/engine.h"
#include "runtime/app/engine_internal.h"

// Map discovery on the live world: standing somewhere uncovers it, and the F5
// window shows what the store holds. A resumed savegame fills the same store
// (savegame_load.cc), so a loaded map and a walked one read alike.
namespace rx {
namespace {

constexpr u32 kEdid = FourCc('E', 'D', 'I', 'D');
constexpr u32 kFull = FourCc('F', 'U', 'L', 'L');

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

}  // namespace

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
