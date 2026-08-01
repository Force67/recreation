#include "bethesda/strings.h"

#include <cstring>

#include "core/log.h"

namespace rx::bethesda {

bool StringTable::Load(const asset::Vfs& vfs, const std::string& plugin_name,
                       const std::string& language, u16 plugin) {
  std::string base = plugin_name.substr(0, plugin_name.rfind('.'));
  std::string prefix = "strings/" + base + "_" + language;
  bool any = LoadFile(vfs, prefix + ".strings", false, plugin);
  any |= LoadFile(vfs, prefix + ".dlstrings", true, plugin);
  any |= LoadFile(vfs, prefix + ".ilstrings", true, plugin);
  return any;
}

const base::String* StringTable::Find(u32 string_id) const {
  return strings_.find(string_id);
}

const base::String* StringTable::Find(u32 string_id, u16 plugin) const {
  if (plugin != kAnyPlugin)
    if (const base::String* s = by_plugin_.find(static_cast<u64>(plugin) << 32 | string_id))
      return s;
  return strings_.find(string_id);
}

bool StringTable::LoadFile(const asset::Vfs& vfs, const std::string& path, bool length_prefixed,
                           u16 plugin) {
  auto bytes = vfs.Read(path);
  if (!bytes || bytes->size() < 8) return false;

  u32 count, data_size;
  std::memcpy(&count, bytes->data(), 4);
  std::memcpy(&data_size, bytes->data() + 4, 4);

  size_t directory_end = 8 + static_cast<size_t>(count) * 8;
  if (bytes->size() < directory_end + data_size) {
    RX_WARN("truncated string table: {}", path);
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
      const size_t chars = static_cast<size_t>(length > 0 ? length - 1 : 0);
      strings_.emplace(id, start + 4, chars);
      if (plugin != kAnyPlugin)
        by_plugin_.emplace(static_cast<u64>(plugin) << 32 | id, base::String(start + 4, chars));
    } else {
      size_t max_length = bytes->size() - pos;
      const size_t chars = strnlen(start, max_length);
      strings_.emplace(id, start, chars);
      if (plugin != kAnyPlugin)
        by_plugin_.emplace(static_cast<u64>(plugin) << 32 | id, base::String(start, chars));
    }
  }
  return true;
}

}  // namespace rx::bethesda
