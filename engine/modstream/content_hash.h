#ifndef RECREATION_MODSTREAM_CONTENT_HASH_H_
#define RECREATION_MODSTREAM_CONTENT_HASH_H_

#include <filesystem>
#include <optional>

#include "core/types.h"
#include "modstream/mod_resource.h"

namespace rx::modstream {

// FNV-1a/64 over a byte range. Matches the constants core/types.h uses for asset
// ids, so the same content hashes the same everywhere in the engine.
ContentHash HashBytes(const void* data, size_t size);

// Streams `path` through the same hash without ever holding the whole file in
// memory, so it scales to multi-hundred-megabyte mod assets. Returns nullopt if
// the file cannot be opened or read.
std::optional<ContentHash> HashFile(const std::filesystem::path& path);

// Incremental FNV-1a state, for hashing a file as its bytes arrive in chunks.
// Seed, Update for each chunk, read `value` when done.
struct ContentHasher {
  ContentHash value = 0xcbf29ce484222325ull;
  void Update(const void* data, size_t size);
};

}  // namespace rx::modstream

#endif  // RECREATION_MODSTREAM_CONTENT_HASH_H_
