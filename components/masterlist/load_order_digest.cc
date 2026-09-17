#include "components/masterlist/load_order_digest.h"

#include <cstdio>

namespace rx::masterlist {
namespace {

// FNV-1a/64, the same hash the mod streamer identifies content with. This is a
// fingerprint for comparison, not a security primitive: nothing is trusted on
// the strength of it, it only says "these two load orders are not the same".
constexpr u64 kFnvOffset = 14695981039346656037ull;
constexpr u64 kFnvPrime = 1099511628211ull;

char Lower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

base::String LoadOrderDigest(const base::Vector<base::String>& plugins) {
  u64 hash = kFnvOffset;
  for (mem_size i = 0; i < plugins.size(); ++i) {
    const base::String& plugin = plugins[i];
    for (mem_size k = 0; k < plugin.size(); ++k) {
      hash ^= static_cast<u64>(static_cast<unsigned char>(Lower(plugin[k])));
      hash *= kFnvPrime;
    }
    // A separator, so ["ab","c"] and ["a","bc"] cannot digest alike.
    hash ^= static_cast<u64>('\n');
    hash *= kFnvPrime;
  }
  char buffer[24] = {};
  std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
  return base::String(buffer);
}

}  // namespace rx::masterlist
