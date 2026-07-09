#ifndef RECREATION_BETHESDA_PLUGIN_H_
#define RECREATION_BETHESDA_PLUGIN_H_

#include <functional>
#include <optional>
#include <string>

#include <base/containers/vector.h>

#include "bethesda/game_profile.h"
#include "bethesda/record.h"
#include "core/types.h"

namespace rx::bethesda {

constexpr u32 kPluginFlagMaster = 0x00000001;
constexpr u32 kPluginFlagLocalized = 0x00000080;
constexpr u32 kPluginFlagLight = 0x00000200;

// Where in the group hierarchy a record sits. Cells and their references
// only make sense relative to the enclosing worldspace and cell children
// groups, which the flattened walk tracks via the group labels.
struct GroupContext {
  RawFormId worldspace;     // label of the enclosing world children group (type 1)
  RawFormId cell;           // label of the enclosing cell children group (type 6/8/9)
  i32 cell_group_type = 0;  // 8 persistent, 9 temporary
  RawFormId dialogue;       // label of the enclosing topic children group (type 7)
};

// Decompresses (if flagged) and splits a record payload into subrecords.
bool ParseRecordPayload(const RecordHeader& header, ByteSpan payload, Record* out);

// One ESM/ESP/ESL file. Owns the file bytes, parses the TES4 header eagerly
// and iterates records on demand. The ESL flag and the .esl extension both
// mark a light plugin.
class PluginFile {
 public:
  static std::optional<PluginFile> Open(const std::string& path, const GameProfile& profile);

  using RecordVisitor = std::function<void(Record& record)>;
  using RawRecordVisitor =
      std::function<void(const RecordHeader& header, ByteSpan payload, const GroupContext& ctx)>;

  // Walks every record in every group. Group recursion is flattened, the
  // visitor sees records in file order.
  bool VisitRecords(const RecordVisitor& visitor) const;

  // Same walk without parsing or decompressing; payload is the raw (possibly
  // compressed) record body pointing into the plugin's bytes. This is what
  // the record store builds its lazy index from.
  bool VisitRecordsRaw(const RawRecordVisitor& visitor) const;

  const std::string& file_name() const { return file_name_; }
  const base::Vector<std::string>& masters() const { return masters_; }
  bool is_master() const { return (header_flags_ & kPluginFlagMaster) != 0; }
  bool is_light() const { return is_light_; }
  bool is_localized() const { return (header_flags_ & kPluginFlagLocalized) != 0; }
  f32 version() const { return version_; }
  u32 record_count() const { return record_count_; }

 private:
  PluginFile() = default;

  bool ParseHeader(const GameProfile& profile);

  std::string file_name_;
  base::Vector<u8> data_;
  // Elements stay std::string: master names flow into LoadOrder::IndexOf and
  // std::ranges::find against GameProfile::base_masters.
  base::Vector<std::string> masters_;
  u32 header_flags_ = 0;
  f32 version_ = 0;
  u32 record_count_ = 0;
  bool is_light_ = false;
  size_t records_begin_ = 0;
};

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_PLUGIN_H_
