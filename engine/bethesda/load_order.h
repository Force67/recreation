#ifndef RECREATION_BETHESDA_LOAD_ORDER_H_
#define RECREATION_BETHESDA_LOAD_ORDER_H_

#include <string>
#include <unordered_map>

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "bethesda/plugin.h"

namespace rec::bethesda {

// Plugins in load order. Built from plugins.txt (asterisk prefixed entries
// are enabled) with base game masters forced to the front, like the games do.
class LoadOrder {
 public:
  static LoadOrder FromPluginsTxt(const std::string& plugins_txt_path, const GameProfile& profile);

  void Append(std::string plugin_file_name);

  // Resolves a raw form id from `referencing_plugin` against its master list
  // into a load order independent id.
  GlobalFormId Resolve(RawFormId raw, u16 referencing_plugin,
                       const base::Vector<std::string>& masters) const;

  u16 IndexOf(const std::string& file_name) const;
  const base::Vector<std::string>& plugins() const { return plugins_; }

 private:
  // Elements stay std::string: plugin names come from std::getline and feed
  // std::filesystem style path concatenation.
  base::Vector<std::string> plugins_;
  // std::string keyed map stays STL: std::string lacks the character_type
  // typedef base::UnorderedMap needs for automatic string hashing.
  std::unordered_map<std::string, u16> index_by_name_;
};

// The merged view of all loaded plugins. Conflicts resolve by last loaded
// wins, which is the rule the entire mod ecosystem is built around. Records
// are indexed lazily: the store keeps raw payload spans into the plugin
// bytes and only decompresses/parses when asked, which keeps the multi
// million record base game loadable in a couple of seconds.
class RecordStore {
 public:
  // Loads every enabled plugin and merges records. Returns false if a
  // required master is missing.
  bool LoadAll(const std::string& data_dir, const LoadOrder& order, const GameProfile& profile);

  struct StoredRecord {
    RecordHeader header;
    ByteSpan payload;  // raw, possibly compressed, owned by the plugin
    u16 winning_plugin = 0;
  };

  const StoredRecord* Find(GlobalFormId id) const;
  // Decompresses and splits the winning record into subrecords.
  bool Parse(GlobalFormId id, Record* out) const;
  size_t record_count() const { return records_.size(); }

  // Iterates winning records of one type, e.g. all CELL or all WEAP.
  void EachOfType(u32 fourcc,
                  const std::function<void(GlobalFormId, const StoredRecord&)>& fn) const;

  // Resolves a raw form id found inside a record body against the masters of
  // the plugin that body came from.
  GlobalFormId ResolveFrom(RawFormId raw, u16 plugin) const;

  // Exterior worldspace index built during load: per worldspace, the CELL,
  // LAND and temporary REFR children at each grid coordinate. Keyed by
  // (x << 16 | y) of the cell grid coordinate, ids are packed GlobalFormIds.
  struct ExteriorCell {
    u64 cell = 0;
    u64 land = 0;
    base::Vector<u64> refs;
  };
  static u32 GridKey(i16 x, i16 y) {
    return static_cast<u32>(static_cast<u16>(x)) << 16 | static_cast<u16>(y);
  }
  using ExteriorGrid = base::UnorderedMap<u32, ExteriorCell>;
  const ExteriorGrid* ExteriorCells(GlobalFormId worldspace) const;

  // Finds a worldspace by editor id, e.g. "Tamriel". Invalid plugin 0xffff
  // when not found.
  GlobalFormId FindWorldspace(std::string_view editor_id) const;

 private:
  struct CellGridSlot {
    u64 worldspace = 0;  // packed
    u32 grid_key = 0;
  };

  LoadOrder order_;
  base::Vector<PluginFile> plugins_;             // keeps payload spans alive
  base::Vector<const PluginFile*> by_order_;     // load order index -> plugin, may be null
  base::UnorderedMap<u64, StoredRecord> records_;
  base::UnorderedMap<u32, base::Vector<u64>> by_type_;
  base::UnorderedMap<u64, ExteriorGrid> exterior_;       // worldspace -> grid
  base::UnorderedMap<u64, CellGridSlot> cell_grid_;      // CELL id -> grid slot
};

}  // namespace rec::bethesda

#endif  // RECREATION_BETHESDA_LOAD_ORDER_H_
