#ifndef RECREATION_NET_WAR_MAP_NET_H_
#define RECREATION_NET_WAR_MAP_NET_H_

#include <optional>
#include <string>
#include <vector>

#include "core/types.h"

namespace rx::net {

// One hold on the Civil War board: its display name and controlling side
// (0 neutral, 1 Imperial, 2 Stormcloak).
struct WarMapHold {
  std::string name;
  i32 owner = 0;
};

// The host's Civil War campaign board, replicated server -> client. The
// authoritative campaign C# only runs on the host, so without this a client's
// war map is blank; this hands it the live board so the panel matches the host.
struct WarMapState {
  std::vector<WarMapHold> holds;
  f32 imperial_fraction = 0.0f;
};

// Encodes the board as little-endian:
//   u8 count | f32 imperial_fraction | count x (u8 name_len | name | u8 owner)
// Hold names and the count are capped at 255.
std::vector<u8> EncodeWarMap(const WarMapState& m);

// Inverse of EncodeWarMap. Returns nullopt on any truncation, never reading out
// of bounds.
std::optional<WarMapState> DecodeWarMap(ByteSpan data);

}  // namespace rx::net

#endif  // RECREATION_NET_WAR_MAP_NET_H_
