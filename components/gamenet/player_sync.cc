#include "components/gamenet/player_sync.h"

#include <nanobuf.h>

namespace rx::net {
namespace {

constexpr size_t kAvatarWireSize = 8 + 8;              // u64 net_id | u64 form
constexpr size_t kVitalsWireSize = 8 + 2 + 2 + 1;      // u64 net_id | u16 | u16 | u8

void AppendU64(std::vector<u8>& out, u64 v) {
  u8 buf[8];
  nanobuf::StoreLe<u64>(buf, v);
  out.insert(out.end(), buf, buf + 8);
}

void AppendU16(std::vector<u8>& out, u16 v) {
  u8 buf[2];
  nanobuf::StoreLe<u16>(buf, v);
  out.insert(out.end(), buf, buf + 2);
}

}  // namespace

std::vector<u8> EncodePlayerAvatar(const PlayerAvatarState& avatar) {
  std::vector<u8> out;
  out.reserve(kAvatarWireSize);
  AppendU64(out, avatar.net_id);
  AppendU64(out, avatar.form);
  return out;
}

std::optional<PlayerAvatarState> DecodePlayerAvatar(const u8* data, size_t size) {
  if (size != kAvatarWireSize)
    return std::nullopt;
  return PlayerAvatarState{nanobuf::LoadLe<u64>(data), nanobuf::LoadLe<u64>(data + 8)};
}

std::vector<u8> EncodePlayerVitals(const PlayerVitals& vitals) {
  std::vector<u8> out;
  out.reserve(kVitalsWireSize);
  AppendU64(out, vitals.net_id);
  AppendU16(out, vitals.health);
  AppendU16(out, vitals.max_health);
  out.push_back(vitals.dead ? 1 : 0);
  return out;
}

std::optional<PlayerVitals> DecodePlayerVitals(const u8* data, size_t size) {
  if (size != kVitalsWireSize)
    return std::nullopt;
  return PlayerVitals{nanobuf::LoadLe<u64>(data),
                      nanobuf::LoadLe<u16>(data + 8),
                      nanobuf::LoadLe<u16>(data + 10),
                      nanobuf::LoadLe<u8>(data + 12) != 0};
}

}  // namespace rx::net
