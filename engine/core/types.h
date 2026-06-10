#ifndef RECREATION_CORE_TYPES_H_
#define RECREATION_CORE_TYPES_H_

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rec {

using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using f32 = float;
using f64 = double;

using ByteSpan = std::span<const u8>;

constexpr u64 Fnv1a(std::string_view str) {
  u64 hash = 0xcbf29ce484222325ull;
  for (char c : str) {
    hash ^= static_cast<u8>(c);
    hash *= 0x100000001b3ull;
  }
  return hash;
}

constexpr u32 FourCc(char a, char b, char c, char d) {
  return static_cast<u32>(a) | static_cast<u32>(b) << 8 | static_cast<u32>(c) << 16 |
         static_cast<u32>(d) << 24;
}

}  // namespace rec

#endif  // RECREATION_CORE_TYPES_H_
