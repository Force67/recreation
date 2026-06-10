#ifndef RECREATION_BETHESDA_PLUGIN_H_
#define RECREATION_BETHESDA_PLUGIN_H_

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "bethesda/game_profile.h"
#include "bethesda/record.h"
#include "core/types.h"

namespace rec::bethesda {

constexpr u32 kPluginFlagMaster = 0x00000001;
constexpr u32 kPluginFlagLocalized = 0x00000080;
constexpr u32 kPluginFlagLight = 0x00000200;

// One ESM/ESP/ESL file. Owns the file bytes, parses the TES4 header eagerly
// and iterates records on demand. The ESL flag and the .esl extension both
// mark a light plugin.
class PluginFile {
 public:
  static std::optional<PluginFile> Open(const std::string& path, const GameProfile& profile);

  using RecordVisitor = std::function<void(Record& record)>;

  // Walks every record in every group. Group recursion is flattened, the
  // visitor sees records in file order.
  bool VisitRecords(const RecordVisitor& visitor) const;

  const std::string& file_name() const { return file_name_; }
  const std::vector<std::string>& masters() const { return masters_; }
  bool is_master() const { return (header_flags_ & kPluginFlagMaster) != 0; }
  bool is_light() const { return is_light_; }
  bool is_localized() const { return (header_flags_ & kPluginFlagLocalized) != 0; }
  f32 version() const { return version_; }
  u32 record_count() const { return record_count_; }

 private:
  PluginFile() = default;

  bool ParseHeader(const GameProfile& profile);

  std::string file_name_;
  std::vector<u8> data_;
  std::vector<std::string> masters_;
  u32 header_flags_ = 0;
  f32 version_ = 0;
  u32 record_count_ = 0;
  bool is_light_ = false;
  size_t records_begin_ = 0;
};

}  // namespace rec::bethesda

#endif  // RECREATION_BETHESDA_PLUGIN_H_
