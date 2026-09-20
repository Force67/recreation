#include "components/gamenet/item_sync.h"

#include <nanobuf.h>

namespace rx::net {
namespace {

constexpr size_t kWireSize = 8 + 8;  // u64 net_id | u64 base

}  // namespace

std::vector<u8> EncodeWorldItem(const WorldItemState& item) {
  std::vector<u8> out(kWireSize);
  nanobuf::StoreLe<u64>(out.data(), item.net_id);
  nanobuf::StoreLe<u64>(out.data() + 8, item.base);
  return out;
}

std::optional<WorldItemState> DecodeWorldItem(const u8* data, size_t size) {
  if (!data || size != kWireSize)
    return std::nullopt;
  return WorldItemState{nanobuf::LoadLe<u64>(data), nanobuf::LoadLe<u64>(data + 8)};
}

}  // namespace rx::net
