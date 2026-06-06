#include "recreation/bethesda/load_order.h"

#include <algorithm>
#include <fstream>

#include "recreation/core/log.h"

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
                                const std::vector<std::string>& masters) const {
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
  plugins_.reserve(order.plugins().size());
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
    plugin->VisitRecords([&](Record& record) {
      GlobalFormId id = order.Resolve(record.header.form_id, i, masters);
      u32 type = record.header.type;
      auto [it, inserted] = records_.try_emplace(id.packed());
      it->second.record = std::move(record);
      it->second.winning_plugin = i;
      if (inserted) by_type_[type].push_back(id.packed());
    });
    plugins_.push_back(std::move(*plugin));
    REC_INFO("loaded {} ({} records total)", name, records_.size());
  }
  return true;
}

const RecordStore::StoredRecord* RecordStore::Find(GlobalFormId id) const {
  auto it = records_.find(id.packed());
  return it == records_.end() ? nullptr : &it->second;
}

void RecordStore::EachOfType(u32 fourcc,
                             const std::function<void(GlobalFormId, const Record&)>& fn) const {
  auto it = by_type_.find(fourcc);
  if (it == by_type_.end()) return;
  for (u64 packed : it->second) {
    const StoredRecord& stored = records_.at(packed);
    fn(GlobalFormId{static_cast<u16>(packed >> 32), static_cast<u32>(packed)}, stored.record);
  }
}

}  // namespace rec::bethesda
