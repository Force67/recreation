#ifndef RECREATION_NET_PLAYER_SYNC_H_
#define RECREATION_NET_PLAYER_SYNC_H_

#include <optional>
#include <vector>

#include "core/types.h"

namespace rx::net {

// The player messages, all on the reliable channel: identity, vitals and
// swings, none of them motion (motion rides the per-tick snapshot stream).
//
// kPlayerAvatar (server -> clients) names which replicated entity is a player's
// body and what it looks like: form 0 means the game's own default player
// template, a non-zero packed GlobalFormId means an NPC_ base the client
// instances instead. The server broadcasts one when a player joins and re-ships
// the whole table to each new joiner, so a late joiner sees everyone already in
// the world.
//
// kPlayerState (server -> clients) carries replicated vitals: the host's combat
// resolution writes them, and so can server-side mod code through the
// Player.SetHealth SDK API.
//
// kPlayerAttack (client -> server) is a swing REQUEST, and carries only the aim
// it was thrown with. A client cannot be trusted to say what it hit -- it does
// not even know where anyone really is, since the host simulates every body --
// so it asks, and the host resolves the swing against the positions it owns.

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

// Fixed 4-byte little-endian record: the f32 yaw the swing was aimed along, in
// the engine's biped convention. The peer it came from is the attacker, so
// nothing else needs to travel.
std::vector<u8> EncodePlayerAttack(f32 yaw);

// Returns nullopt on a buffer of the wrong size, or on a yaw that is not a
// finite number (a NaN would poison every arc test it is fed to).
std::optional<f32> DecodePlayerAttack(const u8* data, size_t size);

}  // namespace rx::net

#endif  // RECREATION_NET_PLAYER_SYNC_H_
