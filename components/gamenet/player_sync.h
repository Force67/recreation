#ifndef RECREATION_NET_PLAYER_SYNC_H_
#define RECREATION_NET_PLAYER_SYNC_H_

#include <optional>
#include <vector>

#include "core/types.h"

namespace rx::net {

// The two player-presence messages, both server -> clients, both riding the
// reliable channel: they are identity/vitals, not motion (motion rides the
// per-tick snapshot stream).
//
// kPlayerAvatar names which replicated entity is a player's body and what it
// looks like: form 0 means the game's own default player template, a non-zero
// packed GlobalFormId means an NPC_ base the client instances instead. The
// server broadcasts one when a player joins and re-ships the whole table to
// each new joiner, so a late joiner sees everyone already in the world.
//
// kPlayerState carries replicated vitals. The engine combat system is not
// networked yet, so the producer today is server-side mod code (the
// Player.SetHealth/Health SDK API); the message and the client-side component
// exist so combat hooks in without another wire change.

struct PlayerAvatarState {
  u64 net_id = 0;
  u64 form = 0;  // packed GlobalFormId of the appearance base; 0 = default
};

struct PlayerVitals {
  u64 net_id = 0;
  u16 health = 0;
  u16 max_health = 0;
  bool dead = false;
};

// Fixed 16-byte little-endian record: u64 net_id | u64 form.
std::vector<u8> EncodePlayerAvatar(const PlayerAvatarState& avatar);

// Returns nullopt on a buffer of the wrong size.
std::optional<PlayerAvatarState> DecodePlayerAvatar(const u8* data, size_t size);

// Fixed 13-byte little-endian record: u64 net_id | u16 health | u16 max | u8 dead.
std::vector<u8> EncodePlayerVitals(const PlayerVitals& vitals);

// Returns nullopt on a buffer of the wrong size.
std::optional<PlayerVitals> DecodePlayerVitals(const u8* data, size_t size);

}  // namespace rx::net

#endif  // RECREATION_NET_PLAYER_SYNC_H_
