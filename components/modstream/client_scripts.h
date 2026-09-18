#ifndef RECREATION_MODSTREAM_CLIENT_SCRIPTS_H_
#define RECREATION_MODSTREAM_CLIENT_SCRIPTS_H_

#include <optional>
#include <string>
#include <vector>

#include "components/modstream/mod_resource.h"
#include "core/types.h"

namespace rx::modstream {

// The client-scripts wire form: alongside the manifest, the server tells a
// joining client which of the streamed files are managed assemblies it asks the
// client to load and run. Each entry pairs a content hash (which locates the
// bytes in the client's content store) with the resource-relative path (which
// names the assembly for logs and the trust prompt). Whether the client agrees
// to execute any of it is the client's script-trust decision, made after this
// message arrives and never assumed by the server.
//
// Shape: a little-endian u32 manifest generation, then a u32 count followed by
// that many {u64 hash, u64 size, u16-length prefixed path} records. The
// generation matches the manifest chunks' own tag, so a delayed duplicate of an
// older offer (a live reload happened meanwhile) is recognized and dropped
// instead of overwriting the current one. The net layer carries the bytes; this
// codec owns the shape so it can be unit-tested apart from any socket.

struct ClientScriptEntry {
  ContentHash hash = 0;
  u64 size = 0;      // file size in bytes, for the consent prompt's summary
  std::string path;  // "resource/relative/file.dll", forward slashes

  bool operator==(const ClientScriptEntry&) const = default;
};

struct ClientScriptOffer {
  u32 generation = 0;
  std::vector<ClientScriptEntry> entries;
};

// Encodes the script list for one client.
std::vector<u8> EncodeClientScripts(u32 generation,
                                    const std::vector<ClientScriptEntry>& scripts);

// Parses a script list received over the wire. Fully bounds-checked: returns
// nullopt on a truncated, oversized (count over max_entries), or malformed
// buffer, so a hostile payload can never make the client over-read or
// over-allocate.
std::optional<ClientScriptOffer> DecodeClientScripts(const u8* data,
                                                     size_t size,
                                                     size_t max_entries);

}  // namespace rx::modstream

#endif  // RECREATION_MODSTREAM_CLIENT_SCRIPTS_H_
