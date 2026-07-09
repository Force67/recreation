#include "bethesda/raw_rewriter.h"

#include <cstring>
#include <fstream>

#include "bethesda/record.h"
#include "core/log.h"

namespace rx::bethesda {
namespace {

constexpr u32 kGrup = FourCc('G', 'R', 'U', 'P');

void PutBytes(base::Vector<u8>* out, const void* data, size_t size) {
  const u8* p = static_cast<const u8*>(data);
  out->insert(out->end(), p, p + size);
}

template <typename T>
void PutPod(base::Vector<u8>* out, const T& v) {
  PutBytes(out, &v, sizeof(T));
}

}  // namespace

std::optional<RawRewriter> RawRewriter::Open(const std::string& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    RX_ERROR("cannot open plugin for rewrite: {}", path);
    return std::nullopt;
  }
  base::Vector<u8> bytes;
  bytes.resize(static_cast<size_t>(file.tellg()));
  file.seekg(0);
  file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return RawRewriter(std::move(bytes));
}

void RawRewriter::Replace(u32 raw_form_id, base::Vector<u8> encoded) {
  replace_[raw_form_id] = std::move(encoded);
}

void RawRewriter::Delete(u32 raw_form_id) { deleted_.insert(raw_form_id); }

void RawRewriter::Insert(u32 record_type, base::Vector<u8> encoded) {
  base::Vector<u8>& group = inserts_[record_type];
  group.insert(group.end(), encoded.begin(), encoded.end());
}

void RawRewriter::EmitRegion(size_t pos, size_t end, base::Vector<u8>* out) const {
  const u8* d = bytes_.data();
  while (pos + sizeof(GroupHeader) <= end) {
    u32 type;
    std::memcpy(&type, d + pos, 4);

    if (type == kGrup) {
      GroupHeader group;
      std::memcpy(&group, d + pos, sizeof(group));
      size_t group_end = pos + group.group_size;
      if (group.group_size < sizeof(GroupHeader) || group_end > end) {
        PutBytes(out, d + pos, end - pos);  // malformed: copy the rest verbatim
        return;
      }
      // Rebuild the body first so the recomputed size covers any edits inside.
      base::Vector<u8> body;
      EmitRegion(pos + sizeof(GroupHeader), group_end, &body);
      GroupHeader emitted = group;
      emitted.group_size = static_cast<u32>(sizeof(GroupHeader) + body.size());
      PutPod(out, emitted);
      PutBytes(out, body.data(), body.size());
      pos = group_end;
      continue;
    }

    RecordHeader header;
    std::memcpy(&header, d + pos, sizeof(header));
    size_t record_end = pos + sizeof(RecordHeader) + header.data_size;
    if (record_end > end) {
      PutBytes(out, d + pos, end - pos);  // truncated: copy the rest verbatim
      return;
    }
    u32 raw = header.form_id.value;
    if (deleted_.count(raw)) {
      // dropped from output
    } else if (auto it = replace_.find(raw); it != replace_.end()) {
      PutBytes(out, it->second.data(), it->second.size());
    } else {
      PutBytes(out, d + pos, record_end - pos);  // verbatim, preserving compression/flags
    }
    pos = record_end;
  }
  // Any trailing bytes shorter than a header (shouldn't happen in a valid file).
  if (pos < end) PutBytes(out, d + pos, end - pos);
}

base::Vector<u8> RawRewriter::Build() const {
  base::Vector<u8> out;
  if (bytes_.size() < sizeof(RecordHeader)) return out;
  const u8* d = bytes_.data();
  const size_t end = bytes_.size();

  // The TES4 header record sits at offset 0, outside any group; copy it whole.
  RecordHeader tes4;
  std::memcpy(&tes4, d, sizeof(tes4));
  size_t tes4_end = sizeof(RecordHeader) + tes4.data_size;
  if (tes4_end > end) tes4_end = end;
  PutBytes(&out, d, tes4_end);

  // Walk the top-level groups so inserts can be appended into the matching type
  // group; group bodies (and any nested edits) go through EmitRegion.
  std::unordered_set<u32> consumed;
  size_t pos = tes4_end;
  while (pos + sizeof(GroupHeader) <= end) {
    u32 type;
    std::memcpy(&type, d + pos, 4);
    if (type != kGrup) break;  // stray top-level record; copy the rest below

    GroupHeader group;
    std::memcpy(&group, d + pos, sizeof(group));
    size_t group_end = pos + group.group_size;
    if (group.group_size < sizeof(GroupHeader) || group_end > end) break;

    base::Vector<u8> body;
    EmitRegion(pos + sizeof(GroupHeader), group_end, &body);
    if (group.group_type == 0) {  // top-level type group: label is the record type
      if (auto it = inserts_.find(group.label); it != inserts_.end()) {
        PutBytes(&body, it->second.data(), it->second.size());
        consumed.insert(group.label);
      }
    }
    GroupHeader emitted = group;
    emitted.group_size = static_cast<u32>(sizeof(GroupHeader) + body.size());
    PutPod(&out, emitted);
    PutBytes(&out, body.data(), body.size());
    pos = group_end;
  }
  if (pos < end) PutBytes(&out, d + pos, end - pos);

  // New top-level type groups for inserts the file had no group for.
  for (const auto& [type, records] : inserts_) {
    if (consumed.count(type)) continue;
    GroupHeader group{};
    group.type = kGrup;
    group.group_size = static_cast<u32>(sizeof(GroupHeader) + records.size());
    group.label = type;
    group.group_type = 0;
    PutPod(&out, group);
    PutBytes(&out, records.data(), records.size());
  }
  return out;
}

bool RawRewriter::Save(const std::string& path) const {
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

}  // namespace rx::bethesda
