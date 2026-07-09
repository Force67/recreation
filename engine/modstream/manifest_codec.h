#ifndef RECREATION_MODSTREAM_MANIFEST_CODEC_H_
#define RECREATION_MODSTREAM_MANIFEST_CODEC_H_

#include <optional>
#include <vector>

#include "core/types.h"
#include "modstream/mod_resource.h"

namespace rx::modstream {

// Serializes a manifest to the compact little-endian wire form the server hands
// every joining client. The bytes ride a normal reliable game packet; this codec
// owns nothing about transport.
std::vector<u8> EncodeManifest(const ModManifest& manifest);

// Parses a manifest from bytes received over the wire. Fully bounds-checked and
// limit-guarded: returns nullopt on a truncated, oversized, or malformed buffer
// so a hostile or corrupt payload can never desync the reader.
std::optional<ModManifest> DecodeManifest(const u8* data, size_t size);

inline std::optional<ModManifest> DecodeManifest(const std::vector<u8>& bytes) {
  return DecodeManifest(bytes.data(), bytes.size());
}

}  // namespace rx::modstream

#endif  // RECREATION_MODSTREAM_MANIFEST_CODEC_H_
