#ifndef RECREATION_NET_ITEM_SYNC_H_
#define RECREATION_NET_ITEM_SYNC_H_

#include <optional>
#include <vector>

#include "core/types.h"

namespace rx::net {

// What a replicated entity is, when it is loot lying on the ground.
//
// The item itself replicates like anything else the host owns: the snapshot
// stream carries its existence and its transform, interest bubbles decide who
// hears about it, and picking it up destroys it on the host, which despawns the
// replica everywhere. This message carries the one thing a snapshot cannot, the
// base record the item came out of, because that is what a client needs to give
// it a mesh. Exactly the shape kPlayerAvatar uses to say which replica is a
// player and what it looks like.
//
// The host keeps the table and re-ships it to a joining client, so somebody who
// arrives later sees the loot already on the floor rather than a scatter of
// invisible transforms.
struct WorldItemState {
  u64 net_id = 0;
  u64 base = 0;  // packed GlobalFormId of the item's base record (WEAP, MISC, ...)
};

// Fixed 16-byte little-endian record: u64 net_id | u64 base.
std::vector<u8> EncodeWorldItem(const WorldItemState& item);

// Returns nullopt on a buffer of the wrong size.
std::optional<WorldItemState> DecodeWorldItem(const u8* data, size_t size);

}  // namespace rx::net

#endif  // RECREATION_NET_ITEM_SYNC_H_
