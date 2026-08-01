#ifndef RECREATION_BETHESDA_STRINGS_H_
#define RECREATION_BETHESDA_STRINGS_H_

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
  // `plugin` is the load-order index the strings belong to. String ids are only
  // unique within one plugin, so the table keeps them per plugin as well as in a
  // flat, first-wins map for callers that do not know which plugin a record is from.
  bool Load(const asset::Vfs& vfs,
            const base::String& plugin_name,
            const base::String& language,
            u16 plugin = kAnyPlugin);

  static constexpr u16 kAnyPlugin = 0xffff;

  const base::String* Find(u32 string_id) const;
  // The string as that plugin wrote it. Falls back to the flat map, so a record
  // from a non-localized plugin still resolves.
  const base::String* Find(u32 string_id, u16 plugin) const;
  size_t size() const { return strings_.size(); }

 private:
  bool LoadFile(const asset::Vfs& vfs, const base::String& path, bool length_prefixed, u16 plugin);

  base::UnorderedMap<u32, base::String> strings_;
  base::UnorderedMap<u64, base::String> by_plugin_;  // (plugin << 32 | id) -> string
};

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_STRINGS_H_
