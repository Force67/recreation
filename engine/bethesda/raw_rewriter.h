#ifndef RECREATION_BETHESDA_RAW_REWRITER_H_
#define RECREATION_BETHESDA_RAW_REWRITER_H_

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <base/containers/vector.h>

#include "core/types.h"

namespace rx::bethesda {

// Structure-preserving rewriter for a single plugin file. Unlike the RecordStore
// load path (which flattens groups, skips deleted records and decompresses), the
// rewriter walks the raw byte structure and copies everything it is not told to
// change verbatim. That makes an unedited rewrite byte-identical to the input
// and preserves what a parse -> re-encode pass loses: original per-record
// compression, deleted records, the exact GRUP tree and ordering, and the whole
// TES4 header (masters, HEDR, ONAM, etc.).
//
// Edits are keyed by the record's on-disk RawFormId value (the 32-bit form id as
// stored in this plugin). Replacing or deleting a record recomputes the size of
// every enclosing group. This is the faithful "modify an existing plugin" path;
// authoring brand new content stays in EditSession/PluginWriter.
class RawRewriter {
 public:
  static std::optional<RawRewriter> Open(const std::string& path);
  explicit RawRewriter(base::Vector<u8> bytes) : bytes_(std::move(bytes)) {}

  // Substitutes a record's full encoded bytes (24-byte header + payload) by its
  // on-disk RawFormId value. `encoded` must be a complete record as produced by
  // EncodeRecord.
  void Replace(u32 raw_form_id, base::Vector<u8> encoded);

  // Drops a record from the output entirely, by its on-disk RawFormId value.
  void Delete(u32 raw_form_id);

  // Adds a brand new record (full encoded bytes) into the top-level GRUP for its
  // record type, creating that group if the plugin has none. Only flat top-level
  // records are supported (not new cell/dialogue children). The HEDR record
  // count is not rewritten, which the games and this loader tolerate.
  void Insert(u32 record_type, base::Vector<u8> encoded);

  // Rewrites the plugin: TES4 verbatim, then the group tree with edits applied
  // and enclosing group sizes recomputed.
  base::Vector<u8> Build() const;
  bool Save(const std::string& path) const;

  const base::Vector<u8>& bytes() const { return bytes_; }

 private:
  void EmitRegion(size_t pos, size_t end, base::Vector<u8>* out) const;

  base::Vector<u8> bytes_;
  std::unordered_map<u32, base::Vector<u8>> replace_;
  std::unordered_set<u32> deleted_;
  std::unordered_map<u32, base::Vector<u8>> inserts_;  // record type -> encoded records
};

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_RAW_REWRITER_H_
