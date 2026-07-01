#ifndef RECREATION_BETHESDA_STRING_WRITER_H_
#define RECREATION_BETHESDA_STRING_WRITER_H_

#include <string>
#include <string_view>

#include <base/containers/vector.h>

#include "core/types.h"

namespace rec::bethesda {

// The write side of the localized string table, the inverse of StringTable
// (strings.cc). A localized plugin stores u32 string ids in its records; the
// text lives in strings/<plugin>_<language>.{strings,dlstrings,ilstrings}. This
// builds those files byte for byte as the reader expects: an 8-byte header
// (u32 count, u32 data_size), a directory of count {u32 id, u32 offset} pairs,
// then the data block that the offsets index into (offsets are relative to the
// end of the directory).
//
// length_prefixed selects the variant, exactly as StringTable::LoadFile does:
//   false -> .strings  : each entry is a NUL-terminated string, no length.
//   true  -> .dlstrings/.ilstrings : each entry is a u32 length (INCLUDING the
//            NUL terminator) followed by the NUL-terminated string.
class StringTableWriter {
 public:
  // Interns `text` and returns its assigned string id. Ids are sequential and
  // start at 1 (0 is reserved by the plugin format to mean "no string").
  u32 Add(std::string_view text);

  // Records `text` under an explicit id, overwriting any prior text for that
  // id. Used when the caller already owns the id space (e.g. ids carried in a
  // record being re-localized). Keeps auto-assignment past the largest id seen.
  void Set(u32 id, std::string_view text);

  // Serializes the whole file. length_prefixed=false for .strings, true for
  // .dlstrings/.ilstrings. Identical strings share a single data-block slot.
  base::Vector<u8> Build(bool length_prefixed) const;

  // Builds and writes the file. Returns false if the file cannot be opened.
  bool Save(const std::string& path, bool length_prefixed) const;

  size_t size() const { return entries_.size(); }

 private:
  struct Entry {
    u32 id;
    std::string text;
  };

  base::Vector<Entry> entries_;
  u32 next_id_ = 1;
};

}  // namespace rec::bethesda

#endif  // RECREATION_BETHESDA_STRING_WRITER_H_
