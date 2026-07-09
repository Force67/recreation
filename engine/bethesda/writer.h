#ifndef RECREATION_BETHESDA_WRITER_H_
#define RECREATION_BETHESDA_WRITER_H_

#include <string>
#include <string_view>

#include <base/containers/vector.h>

#include "bethesda/game_profile.h"
#include "bethesda/record.h"
#include "core/types.h"

namespace rx::bethesda {

// The write side of the plugin module, the inverse of plugin.cc's parser. It
// works on the same Record/Subrecord container the reader produces, so a record
// parsed by ParseRecordPayload can be handed straight back here and, for an
// uncompressed record, re-encodes to identical bytes.
//
// PluginWriter emits flat plugins: a TES4 header plus one top level GRUP per
// record type. Compression is optional (set_compress). Nested CELL/WRLD/DIAL
// group trees are assembled by EditSession, which appends them via
// AddPrebuiltGroup.

// Appends one subrecord in its on-disk [type][u16 size][bytes] form, inverse of
// the reader's ParseSubrecords. Fields larger than 0xffff bytes are preceded by
// the XXXX escape that carries the real 32-bit size.
void EncodeSubrecord(u32 type, ByteSpan data, base::Vector<u8>* out);

// Encodes every subrecord of a record, in order.
void EncodeSubrecords(const base::Vector<Subrecord>& subrecords, base::Vector<u8>* out);

// Encodes one record uncompressed: the 24 byte header followed by its subrecord
// payload, inverse of ParseRecordPayload. The header is preserved verbatim
// except that the compressed flag is cleared and data_size is set to the
// encoded payload length.
void EncodeRecord(const Record& record, base::Vector<u8>* out);

// As above, but when `compress` is set the payload is emitted as a zlib stream
// behind a u32 uncompressed-size prefix and the compressed flag is set, the
// inverse of the reader's DecompressRecord path.
void EncodeRecord(const Record& record, base::Vector<u8>* out, bool compress);

// Writes a GRUP header (group_type, label) wrapping `body`, appending to `out`.
// group_size counts the 24 byte header plus body, as the format requires. This
// is the primitive nested group trees (CELL/WRLD/DIAL children) are built from,
// bottom up.
void EmitGroup(i32 group_type, u32 label, ByteSpan body, base::Vector<u8>* out);

// Builds a flat plugin file in memory.
class PluginWriter {
 public:
  explicit PluginWriter(const GameProfile& profile) : profile_(profile) {}

  // TES4 header configuration. All optional.
  PluginWriter& set_author(std::string author);
  PluginWriter& set_description(std::string description);
  PluginWriter& set_master(bool is_master);  // ESM flag
  PluginWriter& set_light(bool is_light);     // ESL flag
  PluginWriter& set_localized(bool localized);  // strings live in .strings files
  // Appends a master file name; order defines the mod-index references resolve
  // against, exactly like the MAST subrecords the reader parses.
  PluginWriter& add_master(std::string master_file_name);

  // When set, records added via AddRecord are zlib compressed.
  PluginWriter& set_compress(bool compress);

  // Adds a record to be written. It is encoded eagerly, so the record's
  // subrecord spans only need to stay valid for the duration of this call.
  void AddRecord(const Record& record);

  // Appends a fully formed top level group (already carrying its own GRUP
  // header), for nested structures the flat AddRecord path cannot express.
  // `records` is how many records it contains, for HEDR bookkeeping. Prebuilt
  // groups are written after the flat type groups.
  void AddPrebuiltGroup(const base::Vector<u8>& group_bytes, u32 records);

  // Overrides the HEDR next object id (otherwise derived from added records).
  void set_next_object_id(u32 next) {
    next_object_id_ = next;
    next_object_id_pinned_ = true;
  }

  // Serializes the whole plugin: TES4 header, then one top level GRUP per
  // record type in the order the types were first added.
  base::Vector<u8> Build() const;

  // Builds and writes the plugin to disk. Returns false on I/O error.
  bool Save(const std::string& path) const;

  u32 record_count() const { return record_count_; }

 private:
  // Encoded records sharing one top level type, kept in first-seen order so the
  // output group order is deterministic.
  struct Group {
    u32 type = 0;
    base::Vector<u8> records;
  };
  Group& GroupFor(u32 type);

  const GameProfile& profile_;
  std::string author_;
  std::string description_;
  base::Vector<std::string> masters_;
  bool is_master_ = false;
  bool is_light_ = false;
  bool is_localized_ = false;
  bool compress_ = false;
  u32 record_count_ = 0;
  u32 next_object_id_ = 1;  // high-water of added local ids + 1
  bool next_object_id_pinned_ = false;
  base::Vector<Group> groups_;
  base::Vector<u8> prebuilt_;  // nested top level groups, appended after groups_
};

// Authoring helper: accumulates owned field bytes and exposes a Record whose
// subrecord spans point into that storage, ready for PluginWriter::AddRecord.
// Storage buffers are pointer stable (base::Vector move preserves the buffer),
// so spans stay valid as more fields are added.
class RecordBuilder {
 public:
  RecordBuilder(u32 type, RawFormId form_id, u32 flags = 0);

  // Appends a field, copying the bytes into owned storage.
  RecordBuilder& Field(u32 type, ByteSpan bytes);
  // EDID convenience: appends a zero terminated editor id.
  RecordBuilder& EditorId(std::string_view editor_id);
  // Appends a field from a trivially copyable value (little endian on the
  // supported little endian targets, matching how the reader memcpys fields).
  template <typename T>
  RecordBuilder& FieldPod(u32 type, const T& value) {
    return Field(type, ByteSpan(reinterpret_cast<const u8*>(&value), sizeof(T)));
  }

  const Record& record() const { return record_; }

 private:
  Record record_;
  base::Vector<base::Vector<u8>> storage_;
};

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_WRITER_H_
