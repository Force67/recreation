#include "components/bethesda/strings.h"

#include <base/strings/xstring.h>

#include <cstring>

#include "core/log.h"

namespace rx::bethesda {

bool StringTable::Load(const asset::Vfs& vfs,
                       const base::String& plugin_name,
                       const base::String& language,
                       u16 plugin) {
  base::String base = plugin_name.substr(0, plugin_name.rfind('.'));
  base::String prefix = "strings/" + base + "_" + language;
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

bool InterfaceStrings::Load(const asset::Vfs& vfs, const base::String& language) {
  auto bytes = vfs.Read("interface/translate_" + language + ".txt");
  if (!bytes)
    return false;
  return LoadFromBytes(ByteSpan{bytes->data(), bytes->size()});
}

bool InterfaceStrings::LoadFromBytes(ByteSpan utf16le) {
  if (utf16le.size() < 2)
    return false;
  size_t pos = 0;
  if (utf16le[0] == 0xff && utf16le[1] == 0xfe)
    pos = 2;

  // The file is plain UTF-16LE; the shipped tables are all in the BMP, and a
  // surrogate pair would fall through as its two halves rather than corrupt the
  // rest of the line.
  base::String key;
  base::String value;
  bool on_value = false;
  auto flush = [&]() {
    if (!key.empty() && key[0] == '$')
      entries_[key] = value;
    key.clear();
    value.clear();
    on_value = false;
  };

  for (; pos + 1 < utf16le.size(); pos += 2) {
    const u32 code = static_cast<u32>(utf16le[pos]) |
                     (static_cast<u32>(utf16le[pos + 1]) << 8);
    if (code == '\r')
      continue;
    if (code == '\n') {
      flush();
      continue;
    }
    if (code == '\t' && !on_value) {
      on_value = true;
      continue;
    }
    base::String& target = on_value ? value : key;
    if (code < 0x80) {
      target.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      target.push_back(static_cast<char>(0xc0 | (code >> 6)));
      target.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    } else {
      target.push_back(static_cast<char>(0xe0 | (code >> 12)));
      target.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
      target.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    }
  }
  flush();
  return !entries_.empty();
}

base::StringRef InterfaceStrings::Translate(base::StringRef text) const {
  if (text.empty() || text[0] != '$')
    return text;
  if (const base::String* hit = entries_.find(base::String(text)))
    return base::StringRef(*hit);
  return text;
}

bool InterfaceFontConfig::Load(const asset::Vfs& vfs) {
  auto bytes = vfs.Read("interface/fontconfig.txt");
  if (!bytes)
    return false;
  return LoadFromText(base::StringRef(reinterpret_cast<const char*>(bytes->data()),
                                      bytes->size()));
}

bool InterfaceFontConfig::LoadFromText(base::StringRef text) {
  // Two line shapes matter:
  //   fontlib "Interface\\fonts_en.swf"
  //   map "$EverywhereMediumFont" = "Futura Condensed" Normal
  mem_size pos = 0;
  while (pos < text.size()) {
    mem_size end = pos;
    while (end < text.size() && text[end] != '\n')
      ++end;
    base::StringRef line = text.substr(pos, end - pos);
    pos = end + 1;
    while (!line.empty() && (line[line.size() - 1] == '\r' || line[line.size() - 1] == ' '))
      line = line.substr(0, line.size() - 1);
    if (line.empty())
      continue;

    // Collect the quoted fields and whatever bare word trails them.
    base::Vector<base::String> quoted;
    base::String trailing;
    bool in_quotes = false;
    base::String current;
    for (mem_size i = 0; i < line.size(); ++i) {
      const char c = line[i];
      if (c == '"') {
        if (in_quotes) {
          quoted.push_back(base::move(current));
          current = base::String();
        }
        in_quotes = !in_quotes;
        continue;
      }
      if (in_quotes)
        current.push_back(c);
      else if (c != ' ' && c != '=' && c != '\t')
        trailing.push_back(c);
    }

    if (line.size() > 7 && line[0] == 'f' && line[1] == 'o' && line[2] == 'n' &&
        line[3] == 't' && line[4] == 'l' && line[5] == 'i' && line[6] == 'b') {
      if (!quoted.empty()) {
        base::String path;
        for (mem_size i = 0; i < quoted[0].size(); ++i)
          path.push_back(quoted[0][i] == '\\' ? '/' : quoted[0][i]);
        libraries_.push_back(base::move(path));
      }
      continue;
    }
    if (quoted.size() < 2 || line[0] != 'm' || line[1] != 'a' || line[2] != 'p')
      continue;

    // `trailing` holds "map" plus the style word with the separators dropped.
    base::String style;
    for (mem_size i = 3; i < trailing.size(); ++i)
      style.push_back(trailing[i]);
    Mapping mapping;
    mapping.family = quoted[1];
    mapping.bold = style == "Bold" || style == "Demi" || style == "Black";
    mapping.italic = style == "Italic" || style == "Oblique";
    maps_[quoted[0]] = base::move(mapping);
  }
  return !maps_.empty();
}

bool StringTable::LoadFile(const asset::Vfs& vfs,
                           const base::String& path,
                           bool length_prefixed,
                           u16 plugin) {
  auto bytes = vfs.Read(path);
  if (!bytes || bytes->size() < 8)
    return false;

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
    if (pos >= bytes->size())
      continue;

    const char* start = reinterpret_cast<const char*>(bytes->data() + pos);
    if (length_prefixed) {
      if (pos + 4 > bytes->size())
        continue;
      u32 length;
      std::memcpy(&length, start, 4);
      if (pos + 4 + length > bytes->size())
        continue;
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
