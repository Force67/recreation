#include "components/bethesda/string_writer.h"

#include <base/containers/unordered_map.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include <cstring>
#include <fstream>

#include "core/log.h"

namespace rx::bethesda {
namespace {

void PutBytes(base::Vector<u8>* out, const void* data, size_t size) {
  const u8* p = static_cast<const u8*>(data);
  out->insert(out->end(), p, p + size);
}

void PutU32(base::Vector<u8>* out, u32 v) {
  PutBytes(out, &v, sizeof(v));
}

}  // namespace

u32 StringTableWriter::Add(base::StringRef text) {
  u32 id = next_id_++;
  entries_.push_back(Entry{id, base::String(text)});
  return id;
}

void StringTableWriter::Set(u32 id, base::StringRef text) {
  for (Entry& e : entries_) {
    if (e.id == id) {
      e.text.assign(text);
      if (id >= next_id_)
        next_id_ = id + 1;
      return;
    }
  }
  entries_.push_back(Entry{id, base::String(text)});
  if (id >= next_id_)
    next_id_ = id + 1;
}

// GCC's -Wstringop-overflow mis-analyzes base::Vector's byte appends under -O3
// and reports bogus out-of-bounds writes; silence it around the table building.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif

base::Vector<u8> StringTableWriter::Build(bool length_prefixed) const {
  // First lay out the data block. Identical strings are deduplicated to a
  // single slot, so the directory can point several ids at the same offset,
  // matching how shipped tables reuse repeated text.
  base::Vector<u8> data;
  base::Vector<u32> offsets;  // parallel to entries_
  offsets.reserve(entries_.size());
  base::UnorderedMap<base::String, u32> seen;  // text -> data-block offset

  for (const Entry& e : entries_) {
    auto* it = seen.find(e.text);
    if (it != nullptr) {
      offsets.push_back(*it);
      continue;
    }
    u32 offset = static_cast<u32>(data.size());
    if (length_prefixed) {
      // Length INCLUDES the NUL terminator (StringTable reads length - 1).
      PutU32(&data, static_cast<u32>(e.text.size() + 1));
    }
    PutBytes(&data, e.text.data(), e.text.size());
    data.push_back(0);  // NUL terminator (both variants store it)
    seen.emplace(e.text, offset);
    offsets.push_back(offset);
  }

  base::Vector<u8> out;
  const u32 count = static_cast<u32>(entries_.size());
  const u32 data_size = static_cast<u32>(data.size());
  out.reserve(8 + static_cast<size_t>(count) * 8 + data.size());

  PutU32(&out, count);
  PutU32(&out, data_size);
  for (size_t i = 0; i < entries_.size(); ++i) {
    PutU32(&out, entries_[i].id);
    PutU32(&out, offsets[i]);
  }
  PutBytes(&out, data.data(), data.size());
  return out;
}

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

bool StringTableWriter::Save(const base::String& path, bool length_prefixed) const {
  base::Vector<u8> bytes = Build(length_prefixed);
  std::ofstream file(path.c_str(), std::ios::binary | std::ios::trunc);
  if (!file) {
    RX_ERROR("cannot open string table for writing: {}", path);
    return false;
  }
  file.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(file);
}

}  // namespace rx::bethesda
