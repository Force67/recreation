#include "recreation/bethesda/plugin.h"

#include <cstring>
#include <fstream>

#include "recreation/core/log.h"

#if defined(RECREATION_HAS_ZLIB)
#include <zlib.h>
#endif

namespace rec::bethesda {
namespace {

constexpr u32 kTes4 = FourCc('T', 'E', 'S', '4');
constexpr u32 kGrup = FourCc('G', 'R', 'U', 'P');
constexpr u32 kHedr = FourCc('H', 'E', 'D', 'R');
constexpr u32 kMast = FourCc('M', 'A', 'S', 'T');
constexpr u32 kXxxx = FourCc('X', 'X', 'X', 'X');

struct Reader {
  ByteSpan data;
  size_t pos = 0;

  size_t remaining() const { return data.size() - pos; }

  template <typename T>
  bool Read(T* out) {
    if (remaining() < sizeof(T)) return false;
    std::memcpy(out, data.data() + pos, sizeof(T));
    pos += sizeof(T);
    return true;
  }

  bool Skip(size_t count) {
    if (remaining() < count) return false;
    pos += count;
    return true;
  }

  ByteSpan Take(size_t count) {
    ByteSpan span = data.subspan(pos, count);
    pos += count;
    return span;
  }
};

bool ParseSubrecords(ByteSpan data, std::vector<Subrecord>* out) {
  Reader reader{data};
  u32 extended_size = 0;
  while (reader.remaining() >= 6) {
    u32 type = 0;
    u16 size16 = 0;
    reader.Read(&type);
    reader.Read(&size16);
    u32 size = size16;
    if (type == kXxxx) {
      // XXXX carries the true size of the next subrecord.
      if (size16 != 4 || !reader.Read(&extended_size)) return false;
      continue;
    }
    if (extended_size != 0) {
      size = extended_size;
      extended_size = 0;
    }
    if (reader.remaining() < size) return false;
    out->push_back(Subrecord{type, reader.Take(size)});
  }
  return reader.remaining() == 0;
}

bool DecompressRecord(ByteSpan compressed, std::vector<u8>* out) {
  if (compressed.size() < 4) return false;
  u32 uncompressed_size;
  std::memcpy(&uncompressed_size, compressed.data(), 4);
#if defined(RECREATION_HAS_ZLIB)
  out->resize(uncompressed_size);
  uLongf dest_len = uncompressed_size;
  int rc = uncompress(out->data(), &dest_len, compressed.data() + 4,
                      static_cast<uLong>(compressed.size() - 4));
  return rc == Z_OK && dest_len == uncompressed_size;
#else
  REC_WARN("compressed record skipped, built without zlib");
  return false;
#endif
}

bool ParseRecord(const RecordHeader& header, ByteSpan record_data, Record* out) {
  out->header = header;
  ByteSpan payload = record_data;
  if (header.flags & kRecordFlagCompressed) {
    if (!DecompressRecord(record_data, &out->decompressed)) return false;
    payload = ByteSpan(out->decompressed);
  }
  return ParseSubrecords(payload, &out->subrecords);
}

bool EndsWithEsl(const std::string& path) {
  return path.size() >= 4 &&
         (path.ends_with(".esl") || path.ends_with(".ESL") || path.ends_with(".Esl"));
}

}  // namespace

const Subrecord* Record::Find(u32 fourcc) const {
  for (const auto& sub : subrecords) {
    if (sub.type == fourcc) return &sub;
  }
  return nullptr;
}

std::string Record::GetString(u32 fourcc) const {
  const Subrecord* sub = Find(fourcc);
  if (!sub || sub->data.empty()) return {};
  size_t len = sub->data.size();
  if (sub->data[len - 1] == 0) --len;
  return std::string(reinterpret_cast<const char*>(sub->data.data()), len);
}

std::optional<PluginFile> PluginFile::Open(const std::string& path, const GameProfile& profile) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    REC_ERROR("cannot open plugin: {}", path);
    return std::nullopt;
  }
  PluginFile plugin;
  plugin.data_.resize(static_cast<size_t>(file.tellg()));
  file.seekg(0);
  file.read(reinterpret_cast<char*>(plugin.data_.data()),
            static_cast<std::streamsize>(plugin.data_.size()));

  size_t slash = path.find_last_of("/\\");
  plugin.file_name_ = slash == std::string::npos ? path : path.substr(slash + 1);

  if (!plugin.ParseHeader(profile)) {
    REC_ERROR("bad TES4 header: {}", path);
    return std::nullopt;
  }
  if (EndsWithEsl(path)) plugin.is_light_ = true;
  return plugin;
}

bool PluginFile::ParseHeader(const GameProfile& profile) {
  Reader reader{ByteSpan(data_)};
  RecordHeader header;
  if (!reader.Read(&header) || header.type != kTes4) return false;
  if (reader.remaining() < header.data_size) return false;

  header_flags_ = header.flags;
  is_light_ = (header.flags & kPluginFlagLight) != 0;

  Record tes4;
  if (!ParseRecord(header, reader.Take(header.data_size), &tes4)) return false;
  records_begin_ = reader.pos;

  if (const Subrecord* hedr = tes4.Find(kHedr); hedr && hedr->data.size() >= 8) {
    std::memcpy(&version_, hedr->data.data(), 4);
    std::memcpy(&record_count_, hedr->data.data() + 4, 4);
  }
  for (const auto& sub : tes4.subrecords) {
    if (sub.type == kMast && !sub.data.empty()) {
      masters_.emplace_back(reinterpret_cast<const char*>(sub.data.data()), sub.data.size() - 1);
    }
  }

  if (profile.plugin_version != 0 && version_ > profile.plugin_version) {
    REC_WARN("{}: plugin version {} newer than profile {}", file_name_, version_,
             profile.plugin_version);
  }
  return true;
}

bool PluginFile::VisitRecords(const RecordVisitor& visitor) const {
  Reader reader{ByteSpan(data_)};
  reader.pos = records_begin_;

  while (reader.remaining() >= sizeof(GroupHeader)) {
    u32 type;
    std::memcpy(&type, reader.data.data() + reader.pos, 4);

    if (type == kGrup) {
      // Skip the group header and walk its contents inline, which flattens
      // nested groups without recursion.
      if (!reader.Skip(sizeof(GroupHeader))) return false;
      continue;
    }

    RecordHeader header;
    if (!reader.Read(&header) || reader.remaining() < header.data_size) return false;
    ByteSpan payload = reader.Take(header.data_size);
    if (header.flags & kRecordFlagDeleted) continue;

    Record record;
    if (!ParseRecord(header, payload, &record)) {
      REC_WARN("{}: failed to parse record {:08x}", file_name_, header.form_id.value);
      continue;
    }
    visitor(record);
  }
  return true;
}

}  // namespace rec::bethesda
