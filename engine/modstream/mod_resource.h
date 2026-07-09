#ifndef RECREATION_MODSTREAM_MOD_RESOURCE_H_
#define RECREATION_MODSTREAM_MOD_RESOURCE_H_

#include <string>
#include <vector>

#include "core/types.h"

namespace rx::modstream {

// 64-bit content fingerprint (FNV-1a over the file bytes). A file is identified
// by what it contains, so identical files across resources collapse to one
// cache entry and a client only re-downloads what actually changed. This is an
// identity and dedup key, not a security primitive: zetanet's file transporter
// verifies each transfer's bytes end to end on its own.
using ContentHash = u64;

// One file inside a resource: its path relative to the resource root, the byte
// size, and the content hash a client diffs against its local cache.
struct ResourceFile {
  std::string path;  // forward-slash, relative to the resource root
  u64 size = 0;
  ContentHash hash = 0;

  bool operator==(const ResourceFile&) const = default;
};

// A named unit of user content the server distributes, FiveM-style: one
// subdirectory of the server mods directory. Files are kept sorted by path so a
// rebuilt manifest is byte-stable for the same content.
struct ModResource {
  std::string name;
  std::vector<ResourceFile> files;

  bool operator==(const ModResource&) const = default;
};

// The full set of resources a server offers. Sent to every joining client, who
// diffs it against its cache and pulls only the files it is missing.
struct ModManifest {
  std::vector<ModResource> resources;

  u64 TotalBytes() const;
  size_t TotalFiles() const;

  bool operator==(const ModManifest&) const = default;
};

}  // namespace rx::modstream

#endif  // RECREATION_MODSTREAM_MOD_RESOURCE_H_
