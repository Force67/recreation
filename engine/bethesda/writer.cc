#include "bethesda/writer.h"

#include <cstring>
#include <fstream>

#include "bethesda/compression.h"
#include "bethesda/plugin.h"  // plugin flag constants
#include "core/log.h"

namespace rx::bethesda {
namespace {

constexpr u32 kTes4 = FourCc('T', 'E', 'S', '4');
constexpr u32 kGrup = FourCc('G', 'R', 'U', 'P');
constexpr u32 kHedr = FourCc('H', 'E', 'D', 'R');
constexpr u32 kCnam = FourCc('C', 'N', 'A', 'M');
constexpr u32 kSnam = FourCc('S', 'N', 'A', 'M');
constexpr u32 kMast = FourCc('M', 'A', 'S', 'T');
constexpr u32 kData = FourCc('D', 'A', 'T', 'A');
constexpr u32 kXxxx = FourCc('X', 'X', 'X', 'X');

void PutBytes(base::Vector<u8>* out, const void* data, size_t size) {
  const u8* p = static_cast<const u8*>(data);
  out->insert(out->end(), p, p + size);
}

void PutU16(base::Vector<u8>* out, u16 v) { PutBytes(out, &v, sizeof(v)); }
void PutU32(base::Vector<u8>* out, u32 v) { PutBytes(out, &v, sizeof(v)); }

template <typename T>
void PutPod(base::Vector<u8>* out, const T& v) {
  PutBytes(out, &v, sizeof(T));
}

}  // namespace

void EncodeSubrecord(u32 type, ByteSpan data, base::Vector<u8>* out) {
  if (data.size() > 0xffff) {
    // XXXX carries the true 32-bit size; the following field header stores 0.
    PutU32(out, kXxxx);
    PutU16(out, 4);
    PutU32(out, static_cast<u32>(data.size()));
    PutU32(out, type);
    PutU16(out, 0);
  } else {
    PutU32(out, type);
    PutU16(out, static_cast<u16>(data.size()));
  }
  PutBytes(out, data.data(), data.size());
}

void EncodeSubrecords(const base::Vector<Subrecord>& subrecords, base::Vector<u8>* out) {
  for (const auto& sub : subrecords) EncodeSubrecord(sub.type, sub.data, out);
}

void EncodeRecord(const Record& record, base::Vector<u8>* out) {
  EncodeRecord(record, out, /*compress=*/false);
}

void EncodeRecord(const Record& record, base::Vector<u8>* out, bool compress) {
  base::Vector<u8> payload;
  EncodeSubrecords(record.subrecords, &payload);

  RecordHeader header = record.header;
  if (compress && !payload.empty()) {
    base::Vector<u8> stream = ZlibDeflate(ByteSpan(payload.data(), payload.size()));
    header.flags |= kRecordFlagCompressed;
    // On disk: u32 uncompressed size, then the zlib stream.
    header.data_size = static_cast<u32>(4 + stream.size());
    PutPod(out, header);
    PutU32(out, static_cast<u32>(payload.size()));
    PutBytes(out, stream.data(), stream.size());
  } else {
    header.flags &= ~kRecordFlagCompressed;
    header.data_size = static_cast<u32>(payload.size());
    PutPod(out, header);
    PutBytes(out, payload.data(), payload.size());
  }
}

void EmitGroup(i32 group_type, u32 label, ByteSpan body, base::Vector<u8>* out) {
  GroupHeader header{};
  header.type = kGrup;
  header.group_size = static_cast<u32>(sizeof(GroupHeader) + body.size());
  header.label = label;
  header.group_type = group_type;
  PutPod(out, header);
  PutBytes(out, body.data(), body.size());
}

PluginWriter& PluginWriter::set_author(std::string author) {
  author_ = std::move(author);
  return *this;
}

PluginWriter& PluginWriter::set_description(std::string description) {
  description_ = std::move(description);
  return *this;
}

PluginWriter& PluginWriter::set_master(bool is_master) {
  is_master_ = is_master;
  return *this;
}

PluginWriter& PluginWriter::set_light(bool is_light) {
  is_light_ = is_light;
  return *this;
}

PluginWriter& PluginWriter::set_localized(bool localized) {
  is_localized_ = localized;
  return *this;
}

PluginWriter& PluginWriter::add_master(std::string master_file_name) {
  masters_.push_back(std::move(master_file_name));
  return *this;
}

PluginWriter& PluginWriter::set_compress(bool compress) {
  compress_ = compress;
  return *this;
}

PluginWriter::Group& PluginWriter::GroupFor(u32 type) {
  for (auto& group : groups_) {
    if (group.type == type) return group;
  }
  groups_.push_back(Group{type, {}});
  return groups_[groups_.size() - 1];
}

void PluginWriter::AddRecord(const Record& record) {
  EncodeRecord(record, &GroupFor(record.header.type).records, compress_);
  ++record_count_;
  // Track the next free local form id, ignoring references into masters.
  RawFormId id = record.header.form_id;
  if (!next_object_id_pinned_ && !id.is_esl_slot() && id.mod_index() == masters_.size() &&
      id.local_id() + 1 > next_object_id_) {
    next_object_id_ = id.local_id() + 1;
  }
}

void PluginWriter::AddPrebuiltGroup(const base::Vector<u8>& group_bytes, u32 records) {
  PutBytes(&prebuilt_, group_bytes.data(), group_bytes.size());
  record_count_ += records;
}

base::Vector<u8> PluginWriter::Build() const {
  // TES4 payload: HEDR, optional CNAM/SNAM, then MAST+DATA per master.
  base::Vector<u8> tes4_payload;
  {
    u8 hedr[12];
    f32 version = profile_.plugin_version != 0 ? profile_.plugin_version : 1.0f;
    std::memcpy(hedr + 0, &version, 4);
    std::memcpy(hedr + 4, &record_count_, 4);
    std::memcpy(hedr + 8, &next_object_id_, 4);
    EncodeSubrecord(kHedr, ByteSpan(hedr, sizeof(hedr)), &tes4_payload);
  }
  if (!author_.empty()) {
    EncodeSubrecord(kCnam, ByteSpan(reinterpret_cast<const u8*>(author_.c_str()),
                                    author_.size() + 1),
                    &tes4_payload);
  }
  if (!description_.empty()) {
    EncodeSubrecord(kSnam, ByteSpan(reinterpret_cast<const u8*>(description_.c_str()),
                                    description_.size() + 1),
                    &tes4_payload);
  }
  for (const std::string& master : masters_) {
    EncodeSubrecord(kMast, ByteSpan(reinterpret_cast<const u8*>(master.c_str()),
                                    master.size() + 1),
                    &tes4_payload);
    u64 data = 0;  // DATA is an unused 64-bit companion to each MAST.
    EncodeSubrecord(kData, ByteSpan(reinterpret_cast<const u8*>(&data), sizeof(data)),
                    &tes4_payload);
  }

  base::Vector<u8> out;

  RecordHeader tes4{};
  tes4.type = kTes4;
  tes4.data_size = static_cast<u32>(tes4_payload.size());
  tes4.flags = (is_master_ ? kPluginFlagMaster : 0u) | (is_light_ ? kPluginFlagLight : 0u) |
               (is_localized_ ? kPluginFlagLocalized : 0u);
  PutPod(&out, tes4);
  PutBytes(&out, tes4_payload.data(), tes4_payload.size());

  for (const Group& group : groups_) {
    GroupHeader header{};
    header.type = kGrup;
    // group_size covers the 24 byte GRUP header plus everything it contains.
    header.group_size = static_cast<u32>(sizeof(GroupHeader) + group.records.size());
    header.label = group.type;  // top level group label is the record type
    header.group_type = 0;
    PutPod(&out, header);
    PutBytes(&out, group.records.data(), group.records.size());
  }
  PutBytes(&out, prebuilt_.data(), prebuilt_.size());
  return out;
}

bool PluginWriter::Save(const std::string& path) const {
  base::Vector<u8> bytes = Build();
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    RX_ERROR("cannot open plugin for writing: {}", path);
    return false;
  }
  file.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(file);
}

RecordBuilder::RecordBuilder(u32 type, RawFormId form_id, u32 flags) {
  record_.header.type = type;
  record_.header.form_id = form_id;
  record_.header.flags = flags;
}

RecordBuilder& RecordBuilder::Field(u32 type, ByteSpan bytes) {
  base::Vector<u8> buffer;
  buffer.insert(buffer.end(), bytes.begin(), bytes.end());
  base::Vector<u8>& owned = storage_.emplace_back(std::move(buffer));
  record_.subrecords.push_back(Subrecord{type, ByteSpan(owned.data(), owned.size())});
  return *this;
}

RecordBuilder& RecordBuilder::EditorId(std::string_view editor_id) {
  base::Vector<u8> buffer;
  buffer.insert(buffer.end(), editor_id.begin(), editor_id.end());
  buffer.push_back(0);
  base::Vector<u8>& owned = storage_.emplace_back(std::move(buffer));
  record_.subrecords.push_back(
      Subrecord{FourCc('E', 'D', 'I', 'D'), ByteSpan(owned.data(), owned.size())});
  return *this;
}

}  // namespace rx::bethesda
