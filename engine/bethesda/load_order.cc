#include "bethesda/load_order.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "core/log.h"

namespace rec::bethesda {
namespace {

std::string ToLower(std::string_view str) {
  std::string out(str);
  std::ranges::transform(out, out.begin(),
                         [](char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

}  // namespace

LoadOrder LoadOrder::FromPluginsTxt(const std::string& plugins_txt_path,
                                    const GameProfile& profile) {
  LoadOrder order;
  for (const auto& master : profile.base_masters) order.Append(master);

  std::ifstream file(plugins_txt_path);
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;
    if (line.back() == '\r') line.pop_back();
    if (line[0] != '*') continue;  // only enabled plugins
    std::string name = line.substr(1);
    if (order.IndexOf(name) == 0xffff) order.Append(std::move(name));
  }
  return order;
}

void LoadOrder::Append(std::string plugin_file_name) {
  index_by_name_.emplace(ToLower(plugin_file_name), static_cast<u16>(plugins_.size()));
  plugins_.push_back(std::move(plugin_file_name));
}

u16 LoadOrder::IndexOf(const std::string& file_name) const {
  auto it = index_by_name_.find(ToLower(file_name));
  return it == index_by_name_.end() ? 0xffff : it->second;
}

GlobalFormId LoadOrder::Resolve(RawFormId raw, u16 referencing_plugin,
                                const base::Vector<std::string>& masters) const {
  // Mod index below the master count points at a master, otherwise the
  // record is defined by the referencing plugin itself. ESL slots cannot be
  // referenced via the FE prefix from inside a plugin's own master table.
  u8 mod_index = raw.mod_index();
  if (mod_index < masters.size()) {
    return {IndexOf(masters[mod_index]), raw.local_id()};
  }
  return {referencing_plugin, raw.local_id()};
}

bool RecordStore::LoadAll(const std::string& data_dir, const LoadOrder& order,
                          const GameProfile& profile) {
  constexpr u32 kCell = FourCc('C', 'E', 'L', 'L');
  constexpr u32 kRefr = FourCc('R', 'E', 'F', 'R');
  constexpr u32 kLand = FourCc('L', 'A', 'N', 'D');
  constexpr u32 kXclc = FourCc('X', 'C', 'L', 'C');

  order_ = order;
  plugins_.reserve(order.plugins().size());
  by_order_.resize(order.plugins().size());
  for (u16 i = 0; i < order.plugins().size(); ++i) {
    const std::string& name = order.plugins()[i];
    auto plugin = PluginFile::Open(data_dir + "/" + name, profile);
    if (!plugin) {
      bool required = std::ranges::find(profile.base_masters, name) != profile.base_masters.end();
      if (required) {
        REC_ERROR("missing required master: {}", name);
        return false;
      }
      REC_WARN("skipping missing plugin: {}", name);
      continue;
    }

    for (const auto& master : plugin->masters()) {
      if (order.IndexOf(master) == 0xffff) {
        REC_ERROR("{} requires missing master {}", name, master);
        return false;
      }
    }

    const auto& masters = plugin->masters();
    plugin->VisitRecordsRaw([&](const RecordHeader& header, ByteSpan payload,
                                const GroupContext& ctx) {
      GlobalFormId id = order.Resolve(header.form_id, i, masters);
      auto [stored, inserted] = records_.emplace(id.packed());
      stored->header = header;
      stored->payload = payload;
      stored->winning_plugin = i;
      if (inserted) by_type_[header.type].push_back(id.packed());

      if (header.type == kCell && ctx.worldspace.value != 0) {
        // Exterior cell: grid coordinate from XCLC, parsed eagerly since the
        // streamer is keyed on it.
        Record record;
        if (!ParseRecordPayload(header, payload, &record)) return;
        const Subrecord* xclc = record.Find(kXclc);
        if (!xclc || xclc->data.size() < 8) return;
        i32 grid[2];
        std::memcpy(grid, xclc->data.data(), 8);
        u64 world = order.Resolve(ctx.worldspace, i, masters).packed();
        u32 grid_key = GridKey(static_cast<i16>(grid[0]), static_cast<i16>(grid[1]));
        exterior_[world].emplace(grid_key).first->cell = id.packed();
        CellGridSlot* slot = cell_grid_.emplace(id.packed()).first;
        slot->worldspace = world;
        slot->grid_key = grid_key;
      } else if ((header.type == kRefr || header.type == kLand) && ctx.cell.value != 0 &&
                 ctx.cell_group_type == 9) {
        // Temporary cell children. Persistent refs (group type 8) hang off
        // the worldspace cell and are skipped for now.
        u64 cell = order.Resolve(ctx.cell, i, masters).packed();
        const CellGridSlot* slot = cell_grid_.find(cell);
        if (!slot) return;
        ExteriorCell* entry = exterior_[slot->worldspace].find(slot->grid_key);
        if (!entry) return;
        if (header.type == kLand) {
          entry->land = id.packed();
        } else if (inserted) {
          // Overridden refs are already listed under their cell.
          entry->refs.push_back(id.packed());
        }
      }
    });
    plugins_.push_back(std::move(*plugin));
    by_order_[i] = &plugins_.back();
    REC_INFO("loaded {} ({} records total)", name, records_.size());
  }
  return true;
}

const RecordStore::StoredRecord* RecordStore::Find(GlobalFormId id) const {
  return records_.find(id.packed());
}

bool RecordStore::Parse(GlobalFormId id, Record* out) const {
  const StoredRecord* stored = records_.find(id.packed());
  if (!stored) return false;
  return ParseRecordPayload(stored->header, stored->payload, out);
}

void RecordStore::EachOfType(
    u32 fourcc, const std::function<void(GlobalFormId, const StoredRecord&)>& fn) const {
  const base::Vector<u64>* ids = by_type_.find(fourcc);
  if (!ids) return;
  for (u64 packed : *ids) {
    const StoredRecord* stored = records_.find(packed);
    if (!stored) continue;
    fn(GlobalFormId{static_cast<u16>(packed >> 32), static_cast<u32>(packed)}, *stored);
  }
}

GlobalFormId RecordStore::ResolveFrom(RawFormId raw, u16 plugin) const {
  if (plugin >= by_order_.size() || !by_order_[plugin]) return {};
  return order_.Resolve(raw, plugin, by_order_[plugin]->masters());
}

const RecordStore::ExteriorGrid* RecordStore::ExteriorCells(GlobalFormId worldspace) const {
  return exterior_.find(worldspace.packed());
}

GlobalFormId RecordStore::FindWorldspace(std::string_view editor_id) const {
  constexpr u32 kWrld = FourCc('W', 'R', 'L', 'D');
  constexpr u32 kEdid = FourCc('E', 'D', 'I', 'D');
  GlobalFormId found;
  EachOfType(kWrld, [&](GlobalFormId id, const StoredRecord& stored) {
    if (found.plugin != 0xffff) return;
    Record record;
    if (!ParseRecordPayload(stored.header, stored.payload, &record)) return;
    if (record.GetString(kEdid) == editor_id) found = id;
  });
  return found;
}

}  // namespace rec::bethesda
