#include "modstream/asset_request.h"

namespace rx::modstream {
namespace {

void PutU32(std::vector<u8>& out, u32 v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}

void PutU64(std::vector<u8>& out, u64 v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}

u32 LoadU32(const u8* p) {
  return static_cast<u32>(p[0]) | static_cast<u32>(p[1]) << 8 |
         static_cast<u32>(p[2]) << 16 | static_cast<u32>(p[3]) << 24;
}

u64 LoadU64(const u8* p) {
  u64 v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<u64>(p[i]) << (8 * i);
  return v;
}

}  // namespace

std::vector<u8> EncodeHashRequest(const std::vector<ContentHash>& hashes) {
  std::vector<u8> out;
  out.reserve(4 + hashes.size() * 8);
  PutU32(out, static_cast<u32>(hashes.size()));
  for (ContentHash hash : hashes) PutU64(out, hash);
  return out;
}

std::optional<std::vector<ContentHash>> DecodeHashRequest(const u8* data, size_t size,
                                                          size_t max_hashes) {
  if (size < 4) return std::nullopt;
  const u32 count = LoadU32(data);
  if (count > max_hashes) return std::nullopt;
  // The body must be exactly count hashes, no more and no less.
  if (size != static_cast<size_t>(4) + static_cast<size_t>(count) * 8) return std::nullopt;

  std::vector<ContentHash> hashes;
  hashes.reserve(count);
  for (u32 i = 0; i < count; ++i) {
    hashes.push_back(LoadU64(data + 4 + static_cast<size_t>(i) * 8));
  }
  return hashes;
}

}  // namespace rx::modstream
