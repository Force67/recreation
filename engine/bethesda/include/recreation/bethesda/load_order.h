#ifndef RECREATION_BETHESDA_LOAD_ORDER_H_
#define RECREATION_BETHESDA_LOAD_ORDER_H_

#include <string>
#include <unordered_map>
#include <vector>

#include "recreation/bethesda/plugin.h"

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
                       const std::vector<std::string>& masters) const;

  u16 IndexOf(const std::string& file_name) const;
  const std::vector<std::string>& plugins() const { return plugins_; }

 private:
  std::vector<std::string> plugins_;
  std::unordered_map<std::string, u16> index_by_name_;
};

// The merged view of all loaded plugins. Conflicts resolve by last loaded
// wins, which is the rule the entire mod ecosystem is built around.
class RecordStore {
 public:
  // Loads every enabled plugin and merges records. Returns false if a
  // required master is missing.
  bool LoadAll(const std::string& data_dir, const LoadOrder& order, const GameProfile& profile);

  struct StoredRecord {
    Record record;
    u16 winning_plugin = 0;
  };

  const StoredRecord* Find(GlobalFormId id) const;
  size_t record_count() const { return records_.size(); }

  // Iterates winning records of one type, e.g. all CELL or all WEAP.
  void EachOfType(u32 fourcc, const std::function<void(GlobalFormId, const Record&)>& fn) const;

 private:
  std::vector<PluginFile> plugins_;  // keeps subrecord spans alive
  std::unordered_map<u64, StoredRecord> records_;
  std::unordered_map<u32, std::vector<u64>> by_type_;
};

}  // namespace rec::bethesda

#endif  // RECREATION_BETHESDA_LOAD_ORDER_H_
