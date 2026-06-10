#include "bethesda/strings.h"

#include <cstring>

#include "core/log.h"

namespace rec::bethesda {

bool StringTable::Load(const asset::Vfs& vfs, const std::string& plugin_name,
                       const std::string& language) {
  std::string base = plugin_name.substr(0, plugin_name.rfind('.'));
  std::string prefix = "strings/" + base + "_" + language;
  bool any = LoadFile(vfs, prefix + ".strings", false);
  any |= LoadFile(vfs, prefix + ".dlstrings", true);
  any |= LoadFile(vfs, prefix + ".ilstrings", true);
  return any;
}

const std::string* StringTable::Find(u32 string_id) const {
  auto it = strings_.find(string_id);
  return it == strings_.end() ? nullptr : &it->second;
}

bool StringTable::LoadFile(const asset::Vfs& vfs, const std::string& path, bool length_prefixed) {
  auto bytes = vfs.Read(path);
  if (!bytes || bytes->size() < 8) return false;

  u32 count, data_size;
  std::memcpy(&count, bytes->data(), 4);
  std::memcpy(&data_size, bytes->data() + 4, 4);

  size_t directory_end = 8 + static_cast<size_t>(count) * 8;
  if (bytes->size() < directory_end + data_size) {
    REC_WARN("truncated string table: {}", path);
    return false;
  }

  for (u32 i = 0; i < count; ++i) {
    u32 id, offset;
    std::memcpy(&id, bytes->data() + 8 + i * 8, 4);
    std::memcpy(&offset, bytes->data() + 8 + i * 8 + 4, 4);
    size_t pos = directory_end + offset;
    if (pos >= bytes->size()) continue;

    const char* start = reinterpret_cast<const char*>(bytes->data() + pos);
    if (length_prefixed) {
      if (pos + 4 > bytes->size()) continue;
      u32 length;
      std::memcpy(&length, start, 4);
      if (pos + 4 + length > bytes->size()) continue;
      // Length includes the terminator.
      strings_.emplace(id, std::string(start + 4, length > 0 ? length - 1 : 0));
    } else {
      size_t max_length = bytes->size() - pos;
      strings_.emplace(id, std::string(start, strnlen(start, max_length)));
    }
  }
  return true;
}

}  // namespace rec::bethesda
