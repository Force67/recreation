#ifndef RECREATION_BETHESDA_LOAD_ORDER_H_
#define RECREATION_BETHESDA_LOAD_ORDER_H_

#include <string>
#include <unordered_map>

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "bethesda/plugin.h"

namespace rx::bethesda {

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

  // The loaded plugin at a load order index, or null if it was missing. The
  // writer needs a plugin's master list to preserve the mod-index prefix when
  // it re-emits that plugin's records into an override plugin.
  const PluginFile* PluginAt(u16 plugin) const {
    return plugin < by_order_.size() ? by_order_[plugin] : nullptr;
  }

  // Exterior worldspace index built during load: per worldspace, the CELL,
  // LAND and REFR children at each grid coordinate. Persistent refs (which
  // hang off the worldspace dummy cell with no grid of their own) are binned
  // by their placement position. Keyed by (x << 16 | y) of the cell grid
  // coordinate, ids are packed GlobalFormIds.
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

  // Finds an interior cell by editor id, e.g. "WhiterunBanneredMare". Parses
  // every CELL record, so this is a one-time startup cost.
  GlobalFormId FindInteriorCell(std::string_view editor_id) const;

  // Finds a global variable (GLOB) by editor id, e.g. "GameHour". Invalid
  // plugin 0xffff when not found. Used to map the time globals onto the clock.
  GlobalFormId FindGlobal(std::string_view editor_id) const;

  // All REFR children (persistent and temporary) of an interior cell.
  const base::Vector<u64>* InteriorRefs(GlobalFormId cell) const;

  // The interior cell a placed reference belongs to, or invalid (plugin
  // 0xffff) when the ref is not an interior child (e.g. an exterior load
  // door). Lets a load door discover the interior to stream on entry.
  GlobalFormId InteriorCellOfRef(GlobalFormId ref) const;

  // The placed actor reference (ACHR) a base NPC_ form is placed as, or invalid
  // when none. Built lazily on first call by scanning every ACHR (a one-time
  // cost), so a unique-actor quest alias can resolve its reference. First
  // placement wins.
  GlobalFormId PlacedRefForBase(GlobalFormId base) const;

  // The INFO response records under a DIAL topic, in file order (the order the
  // engine evaluates them). Null when the topic has no children.
  const base::Vector<u64>* TopicInfos(GlobalFormId dial) const;

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
  base::UnorderedMap<u64, base::Vector<u64>> interior_;  // CELL id -> refs
  base::UnorderedMap<u64, u64> ref_interior_cell_;       // interior REFR id -> CELL id
  // base NPC_ id -> placed ACHR ref id, lazily built (see PlacedRefForBase).
  mutable base::UnorderedMap<u64, u64> base_to_achr_;
  mutable bool base_to_achr_built_ = false;
  base::UnorderedMap<u64, base::Vector<u64>> topic_infos_;  // DIAL id -> INFO ids
};

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_LOAD_ORDER_H_
