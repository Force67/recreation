#ifndef RECREATION_MODSTREAM_ASSET_REQUEST_H_
#define RECREATION_MODSTREAM_ASSET_REQUEST_H_

#include <optional>
#include <vector>

#include "core/types.h"
#include "modstream/mod_resource.h"

namespace rx::modstream {

// The asset-request wire form: a client asks the server to stream a batch of
// content hashes (the files it is missing). A compact little-endian u32 count
// followed by that many u64 hashes. The net layer carries the bytes; this codec
// owns the shape so it can be unit-tested and fuzzed apart from any socket.

// Encodes a batch of hashes. The caller splits a large plan into datagram-sized
// batches; this encodes one batch.
std::vector<u8> EncodeHashRequest(const std::vector<ContentHash>& hashes);

// Parses a request received over the wire. Fully bounds-checked: returns nullopt
// on a truncated, oversized (count over max_hashes), or malformed buffer, so a
// hostile payload can never make the server over-read or over-allocate.
std::optional<std::vector<ContentHash>> DecodeHashRequest(const u8* data, size_t size,
                                                          size_t max_hashes);

}  // namespace rx::modstream

#endif  // RECREATION_MODSTREAM_ASSET_REQUEST_H_
