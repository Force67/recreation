#include "modstream/manifest_chunk.h"

#include <algorithm>

namespace rx::modstream {
namespace {

constexpr size_t kHeaderSize = 16;

void PutU32(std::vector<u8>& out, u32 v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}

u32 LoadU32(const u8* p) {
  return static_cast<u32>(p[0]) | static_cast<u32>(p[1]) << 8 |
         static_cast<u32>(p[2]) << 16 | static_cast<u32>(p[3]) << 24;
}

}  // namespace

u32 ManifestChunkCount(u32 total_size) {
  return (total_size + kManifestChunkPayload - 1) / kManifestChunkPayload;
}

std::vector<u8> EncodeManifestChunk(u32 generation, u32 total_size, u32 total_chunks,
                                    u32 chunk_index, const u8* payload, u32 payload_len) {
  std::vector<u8> out;
  out.reserve(kHeaderSize + payload_len);
  PutU32(out, generation);
  PutU32(out, total_size);
  PutU32(out, total_chunks);
  PutU32(out, chunk_index);
  out.insert(out.end(), payload, payload + payload_len);
  return out;
}

std::optional<ManifestChunkView> DecodeManifestChunk(const u8* data, size_t size) {
  if (size < kHeaderSize) return std::nullopt;

  ManifestChunkView view;
  view.generation = LoadU32(data);
  view.total_size = LoadU32(data + 4);
  view.total_chunks = LoadU32(data + 8);
  view.chunk_index = LoadU32(data + 12);

  if (view.total_size == 0 || view.total_size > kMaxManifestSize) return std::nullopt;
  if (view.total_chunks != ManifestChunkCount(view.total_size)) return std::nullopt;
  if (view.chunk_index >= view.total_chunks) return std::nullopt;

  // The payload for this index is a full chunk, except the last which holds the
  // remainder. The buffer must contain exactly that much.
  const u32 offset = view.chunk_index * kManifestChunkPayload;
  const u32 expected =
      std::min<u32>(kManifestChunkPayload, view.total_size - offset);
  if (size != kHeaderSize + expected) return std::nullopt;

  view.payload = data + kHeaderSize;
  view.payload_len = expected;
  return view;
}

}  // namespace rx::modstream
