#ifndef RECREATION_MODSTREAM_MANIFEST_CHUNK_H_
#define RECREATION_MODSTREAM_MANIFEST_CHUNK_H_

#include <optional>
#include <vector>

#include "core/types.h"

namespace rx::modstream {

// The manifest is too big to ride one datagram, so it is split into chunks. Each
// chunk packet is a 16-byte header (u32 generation, u32 total size, u32 total
// chunk count, u32 chunk index) followed by this chunk's slice of the manifest
// bytes. The client reassembles them into a buffer; the generation rises each
// time the server reloads its mods, so a connected client can tell a fresh
// manifest from a stale retransmit. This codec owns the chunk framing so the
// parse and its validation can be unit-tested and fuzzed apart from the net layer.

// Manifest bytes carried per chunk, well under the ~64 KiB UDP datagram ceiling.
inline constexpr u32 kManifestChunkPayload = 60000;
// Refuse a reassembled manifest larger than this; it bounds the client's buffer
// against a hostile header, and a real catalog is orders of magnitude smaller.
inline constexpr u32 kMaxManifestSize = 64u * 1024 * 1024;

// Number of chunks a manifest of `total_size` bytes splits into (at least 1 when
// total_size is positive).
u32 ManifestChunkCount(u32 total_size);

// A parsed, fully validated chunk: the reassembly parameters plus a borrowed view
// of this chunk's payload bytes within the source buffer.
struct ManifestChunkView {
  u32 generation = 0;
  u32 total_size = 0;
  u32 total_chunks = 0;
  u32 chunk_index = 0;
  const u8* payload = nullptr;
  u32 payload_len = 0;
};

// Encodes one chunk: the header then `payload_len` bytes from `payload`.
std::vector<u8> EncodeManifestChunk(u32 generation, u32 total_size, u32 total_chunks,
                                    u32 chunk_index, const u8* payload, u32 payload_len);

// Parses and validates a received chunk: header present, total positive and under
// the cap, chunk count consistent with the total, index in range, and the payload
// length exactly what that index must carry. Returns nullopt otherwise, so a
// hostile chunk can never drive an out-of-range write during reassembly. The
// returned view borrows `data`.
std::optional<ManifestChunkView> DecodeManifestChunk(const u8* data, size_t size);

}  // namespace rx::modstream

#endif  // RECREATION_MODSTREAM_MANIFEST_CHUNK_H_
