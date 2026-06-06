#ifndef RECREATION_BETHESDA_RECORD_H_
#define RECREATION_BETHESDA_RECORD_H_

#include <string>
#include <vector>

#include "recreation/bethesda/form_id.h"
#include "recreation/core/types.h"

namespace rec::bethesda {

// On disk record header, identical layout in Skyrim SE, FO4 and FO76.
struct RecordHeader {
  u32 type = 0;  // fourcc, e.g. 'WEAP'
  u32 data_size = 0;
  u32 flags = 0;
  RawFormId form_id;
  u32 version_control = 0;
  u16 form_version = 0;
  u16 unknown = 0;
};
static_assert(sizeof(RecordHeader) == 24);

constexpr u32 kRecordFlagCompressed = 0x00040000;
constexpr u32 kRecordFlagDeleted = 0x00000020;

struct GroupHeader {
  u32 type = 0;  // 'GRUP'
  u32 group_size = 0;
  u32 label = 0;
  i32 group_type = 0;
  u32 version_control = 0;
  u32 unknown = 0;
};
static_assert(sizeof(GroupHeader) == 24);

struct Subrecord {
  u32 type = 0;
  ByteSpan data;
};

// A parsed record with its subrecords decoded, data decompressed if needed.
// Subrecord spans point into the owning PluginFile or into `decompressed`,
// so records are move only and must not outlive their plugin.
struct Record {
  Record() = default;
  Record(Record&&) = default;
  Record& operator=(Record&&) = default;
  Record(const Record&) = delete;
  Record& operator=(const Record&) = delete;

  RecordHeader header;
  std::vector<u8> decompressed;  // backing storage when compressed
  std::vector<Subrecord> subrecords;

  const Subrecord* Find(u32 fourcc) const;
  std::string GetString(u32 fourcc) const;  // zero terminated subrecord as string
};

}  // namespace rec::bethesda

#endif  // RECREATION_BETHESDA_RECORD_H_
