#include "modstream/content_hash.h"

#include <array>
#include <fstream>

namespace rx::modstream {
namespace {

constexpr ContentHash kFnvPrime = 0x100000001b3ull;

}  // namespace

void ContentHasher::Update(const void* data, size_t size) {
  const auto* bytes = static_cast<const u8*>(data);
  for (size_t i = 0; i < size; ++i) {
    value ^= bytes[i];
    value *= kFnvPrime;
  }
}

ContentHash HashBytes(const void* data, size_t size) {
  ContentHasher hasher;
  hasher.Update(data, size);
  return hasher.value;
}

std::optional<ContentHash> HashFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;

  ContentHasher hasher;
  std::array<char, 64 * 1024> buffer;
  while (in) {
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize got = in.gcount();
    if (got > 0) hasher.Update(buffer.data(), static_cast<size_t>(got));
  }
  // eof is the expected stop condition; any other failure means a short read.
  if (in.bad()) return std::nullopt;
  return hasher.value;
}

}  // namespace rx::modstream
