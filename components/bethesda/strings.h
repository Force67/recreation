#ifndef RECREATION_BETHESDA_STRINGS_H_
#define RECREATION_BETHESDA_STRINGS_H_

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/strings/string_ref.h>
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

// Interface/translate_<language>.txt: the interface's own key table. Menus put
// a "$KEY" in their text fields and Scaleform swaps in the localized string at
// runtime, which is why a movie read straight off disk is full of "$LEVEL" and
// "$Saving...". UTF-16LE with a BOM, one "$KEY<tab>Text" line per entry.
class InterfaceStrings {
 public:
  // Reads interface/translate_<language>.txt; language is lower case ("english").
  bool Load(const asset::Vfs& vfs, const base::String& language);
  bool LoadFromBytes(ByteSpan utf16le);

  // The text a field should show. A value that is exactly a known key becomes
  // its translation; anything else comes back unchanged, so this is safe to run
  // over every string in a movie.
  base::StringRef Translate(base::StringRef text) const;

  size_t size() const { return entries_.size(); }
  const base::UnorderedMap<base::String, base::String>& entries() const {
    return entries_;
  }

 private:
  base::UnorderedMap<base::String, base::String> entries_;
};

// Interface/fontconfig.txt: which real typeface each "$Font" symbol means. The
// menus never name a font directly; they name a symbol, and this table maps it
// onto a family and a style inside one of the font movies the same file lists.
class InterfaceFontConfig {
 public:
  struct Mapping {
    base::String family;  // e.g. "Futura Condensed"
    bool bold = false;
    bool italic = false;
  };

  bool Load(const asset::Vfs& vfs);
  bool LoadFromText(base::StringRef text);

  const Mapping* Find(const base::String& symbol) const { return maps_.find(symbol); }
  const base::UnorderedMap<base::String, Mapping>& maps() const { return maps_; }
  // The font movies the config pulls its faces from, in listed order.
  const base::Vector<base::String>& libraries() const { return libraries_; }

 private:
  base::UnorderedMap<base::String, Mapping> maps_;
  base::Vector<base::String> libraries_;
};

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_STRINGS_H_
