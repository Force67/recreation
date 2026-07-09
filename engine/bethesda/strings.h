#ifndef RECREATION_BETHESDA_STRINGS_H_
#define RECREATION_BETHESDA_STRINGS_H_

#include <string>

#include <base/containers/unordered_map.h>
#include <base/strings/xstring.h>

#include "asset/vfs.h"
#include "core/types.h"

namespace rx::bethesda {

// Localized plugins store string ids instead of inline text. The actual
// strings live in strings/<plugin>_<language>.strings (.dlstrings and
// .ilstrings for length prefixed variants).
class StringTable {
 public:
  bool Load(const asset::Vfs& vfs, const std::string& plugin_name, const std::string& language);

  const base::String* Find(u32 string_id) const;
  size_t size() const { return strings_.size(); }

 private:
  bool LoadFile(const asset::Vfs& vfs, const std::string& path, bool length_prefixed);

  base::UnorderedMap<u32, base::String> strings_;
};

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_STRINGS_H_
