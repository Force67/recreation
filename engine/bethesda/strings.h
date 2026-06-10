#ifndef RECREATION_BETHESDA_STRINGS_H_
#define RECREATION_BETHESDA_STRINGS_H_

#include <string>
#include <unordered_map>

#include "asset/vfs.h"
#include "core/types.h"

namespace rec::bethesda {

// Localized plugins store string ids instead of inline text. The actual
// strings live in strings/<plugin>_<language>.strings (.dlstrings and
// .ilstrings for length prefixed variants).
class StringTable {
 public:
  bool Load(const asset::Vfs& vfs, const std::string& plugin_name, const std::string& language);

  const std::string* Find(u32 string_id) const;
  size_t size() const { return strings_.size(); }

 private:
  bool LoadFile(const asset::Vfs& vfs, const std::string& path, bool length_prefixed);

  std::unordered_map<u32, std::string> strings_;
};

}  // namespace rec::bethesda

#endif  // RECREATION_BETHESDA_STRINGS_H_
