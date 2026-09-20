#include "components/gamenet/world_state.h"

#include <nanobuf.h>

namespace rx::net {
namespace {

constexpr size_t kWireSize = 8 + 4 + 8 + 8;

}  // namespace

std::vector<u8> EncodeWorldState(const WorldState& state) {
  std::vector<u8> out(kWireSize);
  u8* p = out.data();
  nanobuf::StoreLe<u64>(p, nanobuf::BitsOf(state.game_days));
  nanobuf::StoreLe<u32>(p + 8, nanobuf::BitsOf(state.timescale));
  nanobuf::StoreLe<u64>(p + 12, state.weather_seed);
  nanobuf::StoreLe<u64>(p + 20, state.weather);
  return out;
}

std::optional<WorldState> DecodeWorldState(const u8* data, size_t size) {
  if (!data || size != kWireSize)
    return std::nullopt;
  WorldState state;
  state.game_days = nanobuf::DoubleFromBits(nanobuf::LoadLe<u64>(data));
  state.timescale = nanobuf::FloatFromBits(nanobuf::LoadLe<u32>(data + 8));
  state.weather_seed = nanobuf::LoadLe<u64>(data + 12);
  state.weather = nanobuf::LoadLe<u64>(data + 20);
  return state;
}

}  // namespace rx::net
