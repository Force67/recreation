#ifndef RECREATION_NET_WORLD_REPLICATION_H_
#define RECREATION_NET_WORLD_REPLICATION_H_

#include <base/containers/vector.h>
#include <base/optional.h>

#include <optional>
#include <vector>

#include "components/world/quest_world.h"
#include "core/types.h"

namespace rx::net {

inline constexpr size_t kMaxWorldCommandsPerMessage = 1024;
inline constexpr size_t kMaxWorldCommandPayload = 256 * 1024;

// Replication for quest-driven world commands (spawn / move / enable / delete /
// cleanup). The host drains its WorldCommandQueue each tick and ships the list
// on the reliable channel; every client applies the identical list to its own
// QuestWorld, so spawned NPCs, moved references, disables, and quest cleanup are
// mirrored exactly -- and the provenance ledger stays consistent on all peers.
//
// The spawn mesh is never sent: clients resolve it from the base form locally
// (the same way quest text is resolved locally in quest_replication). rotation
// and scale are likewise omitted for now and default on the client.
std::vector<u8> EncodeWorldCommands(const std::vector<world::WorldCommand>& commands);

// Inverse of EncodeWorldCommands. Returns nullopt on a truncated or corrupt
// blob, never reading out of bounds.
base::Optional<base::Vector<world::WorldCommand>> DecodeWorldCommands(ByteSpan data);

}  // namespace rx::net

#endif  // RECREATION_NET_WORLD_REPLICATION_H_
