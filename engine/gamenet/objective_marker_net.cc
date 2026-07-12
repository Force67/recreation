#include "gamenet/objective_marker_net.h"

#include <nanobuf.h>

namespace rx::net {
namespace {

// Fixed wire layout, all little-endian:
//   u8 active | u64 quest | f32 x | f32 y | f32 z
constexpr size_t kWireSize = 1 + 8 + 4 + 4 + 4;

}  // namespace

std::vector<u8> EncodeObjectiveMarker(const ObjectiveMarkerState& m) {
  std::vector<u8> out(kWireSize);
  u8* p = out.data();
  p[0] = m.active ? 1 : 0;
  nanobuf::StoreLe<u64>(p + 1, m.quest);
  nanobuf::StoreLe<u32>(p + 9, nanobuf::BitsOf(m.x));
  nanobuf::StoreLe<u32>(p + 13, nanobuf::BitsOf(m.y));
  nanobuf::StoreLe<u32>(p + 17, nanobuf::BitsOf(m.z));
  return out;
}

std::optional<ObjectiveMarkerState> DecodeObjectiveMarker(ByteSpan data) {
  if (data.size() != kWireSize) return std::nullopt;
  const u8* p = data.data();
  ObjectiveMarkerState m;
  m.active = p[0] != 0;
  m.quest = nanobuf::LoadLe<u64>(p + 1);
  m.x = nanobuf::FloatFromBits(nanobuf::LoadLe<u32>(p + 9));
  m.y = nanobuf::FloatFromBits(nanobuf::LoadLe<u32>(p + 13));
  m.z = nanobuf::FloatFromBits(nanobuf::LoadLe<u32>(p + 17));
  return m;
}

}  // namespace rx::net
