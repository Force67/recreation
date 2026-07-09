#include "net/war_map_net.h"

#include <algorithm>

#include <nanobuf.h>

namespace rx::net {

std::vector<u8> EncodeWarMap(const WarMapState& m) {
  std::vector<u8> out;
  const u8 count = static_cast<u8>(std::min<size_t>(m.holds.size(), 255));
  out.push_back(count);
  u8 frac[4];
  nanobuf::StoreLe<u32>(frac, nanobuf::BitsOf(m.imperial_fraction));
  out.insert(out.end(), frac, frac + 4);
  for (u8 i = 0; i < count; ++i) {
    const WarMapHold& h = m.holds[i];
    const u8 len = static_cast<u8>(std::min<size_t>(h.name.size(), 255));
    out.push_back(len);
    out.insert(out.end(), h.name.begin(), h.name.begin() + len);
    out.push_back(static_cast<u8>(h.owner));
  }
  return out;
}

std::optional<WarMapState> DecodeWarMap(ByteSpan data) {
  if (data.size() < 5) return std::nullopt;  // count + fraction
  const u8* p = data.data();
  size_t off = 0;
  const u8 count = p[off++];
  WarMapState m;
  m.imperial_fraction = nanobuf::FloatFromBits(nanobuf::LoadLe<u32>(p + off));
  off += 4;
  m.holds.reserve(count);
  for (u8 i = 0; i < count; ++i) {
    if (off >= data.size()) return std::nullopt;
    const u8 len = p[off++];
    if (off + static_cast<size_t>(len) + 1 > data.size()) return std::nullopt;  // name + owner
    WarMapHold h;
    h.name.assign(reinterpret_cast<const char*>(p + off), len);
    off += len;
    h.owner = static_cast<i8>(p[off++]);
    m.holds.push_back(std::move(h));
  }
  return m;
}

}  // namespace rx::net
