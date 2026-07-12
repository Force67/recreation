#ifndef RECREATION_NET_OBJECTIVE_MARKER_NET_H_
#define RECREATION_NET_OBJECTIVE_MARKER_NET_H_

#include <optional>
#include <vector>

#include "core/types.h"

namespace rx::net {

// The host's single active objective waypoint (the quest compass marker),
// replicated server -> client so a client's HUD can draw the same pip. Only
// the one active marker travels; the client computes the on-screen pip from
// the world position and its own camera.
struct ObjectiveMarkerState {
  bool active = false;       // false hides the client's pip
  u64 quest = 0;             // packed GlobalFormId of the marker's quest
  f32 x = 0, y = 0, z = 0;   // marker world position (engine space)
};

// Encodes the marker as a fixed 21-byte little-endian record:
//   u8 active | u64 quest | f32 x | f32 y | f32 z
// matching the nanobuf runtime's codec (floats as their u32 bit pattern).
std::vector<u8> EncodeObjectiveMarker(const ObjectiveMarkerState& m);

// Inverse of EncodeObjectiveMarker. Returns nullopt unless `data` is exactly
// 21 bytes, never reading out of bounds.
std::optional<ObjectiveMarkerState> DecodeObjectiveMarker(ByteSpan data);

}  // namespace rx::net

#endif  // RECREATION_NET_OBJECTIVE_MARKER_NET_H_
