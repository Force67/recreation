#ifndef RECREATION_MODSTREAM_CONTENT_STORE_H_
#define RECREATION_MODSTREAM_CONTENT_STORE_H_

#include <filesystem>
#include <optional>
#include <vector>

#include "modstream/mod_resource.h"

namespace rec::modstream {

// The client side of asset streaming: a content-addressed cache on disk. Files
// live under root/<shard>/<hash>.bin keyed by their content hash, so identical
// bytes shared across resources or servers are stored once and a client only
// ever downloads content it does not already hold. Entries are immutable: a hash
// always names the same bytes.
class ContentStore {
 public:
  explicit ContentStore(std::filesystem::path root);

  // True if the cache already holds content with this hash.
  bool Has(ContentHash hash) const;

  // Absolute path of the cached file, or nullopt if absent.
  std::optional<std::filesystem::path> PathFor(ContentHash hash) const;

  // Writes `bytes` into the cache after verifying they hash to `expected`.
  // Rejects (returns nullopt) on a hash mismatch so corrupt or spoofed content
  // can never enter the store. The write is atomic: a temp file is renamed into
  // place only once complete.
  std::optional<std::filesystem::path> Store(ContentHash expected,
                                             const std::vector<u8>& bytes);

  // Takes ownership of a finished transfer's file: re-hashes it from disk
  // (without loading it whole), moves it into place on a match, and deletes it
  // on a mismatch. Returns the final cache path, or nullopt on mismatch or I/O
  // failure.
  std::optional<std::filesystem::path> Adopt(ContentHash expected,
                                             const std::filesystem::path& source);

  // Takes ownership of a file whose hash is not yet known: hashes it from disk,
  // moves it into the cache under that content hash, and returns the hash. Use
  // this for streamed-in content where identity comes from the bytes themselves,
  // not from a promised id. Returns nullopt on I/O failure; the source file is
  // consumed on success.
  std::optional<ContentHash> Ingest(const std::filesystem::path& source);

  const std::filesystem::path& root() const { return root_; }

 private:
  std::filesystem::path PathOf(ContentHash hash) const;
  bool EnsureShard(ContentHash hash) const;

  std::filesystem::path root_;
};

}  // namespace rec::modstream

#endif  // RECREATION_MODSTREAM_CONTENT_STORE_H_
