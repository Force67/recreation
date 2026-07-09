#ifndef RECREATION_MODSTREAM_MOD_CATALOG_H_
#define RECREATION_MODSTREAM_MOD_CATALOG_H_

#include <filesystem>
#include <optional>
#include <unordered_map>

#include "modstream/mod_resource.h"

namespace rx::modstream {

// The server side of asset streaming: a scan of the mods directory that both
// describes what is offered (the manifest sent to clients) and resolves a
// requested content hash back to the file on disk so the transporter can stream
// it. Built once at startup; the directory is treated as read-only afterward.
class ModCatalog {
 public:
  // Scans `mods_dir`. Each immediate subdirectory is a resource; every file
  // beneath it (recursively) is hashed and recorded under its resource-relative,
  // normalized path. Returns nullopt if the directory does not exist or a file
  // cannot be read, so a misconfigured server fails loudly instead of offering a
  // manifest that lies about what it can serve. An existing but empty directory
  // yields an empty catalog (a valid "nothing to stream").
  static std::optional<ModCatalog> Build(const std::filesystem::path& mods_dir);

  const ModManifest& manifest() const { return manifest_; }

  // Absolute on-disk path of the file with this content hash, or nullopt if no
  // catalogued file has it. The server consults this before honoring a client's
  // request, so a client can never pull a path outside the mods directory.
  std::optional<std::filesystem::path> PathForHash(ContentHash hash) const;

 private:
  ModManifest manifest_;
  std::unordered_map<ContentHash, std::filesystem::path> by_hash_;
};

}  // namespace rx::modstream

#endif  // RECREATION_MODSTREAM_MOD_CATALOG_H_
