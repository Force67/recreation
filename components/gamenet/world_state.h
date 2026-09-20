#ifndef RECREATION_NET_WORLD_STATE_H_
#define RECREATION_NET_WORLD_STATE_H_

#include <optional>
#include <vector>

#include "core/types.h"

namespace rx::net {

// The host's shared world state: what time it is and what the sky is doing.
// Replicated server -> clients because both are facts about the world rather
// than about a player. Nothing syncs them today, and they do not agree on their
// own: each machine starts its clock when IT finished loading and then advances
// it at the game's timescale, so two players who joined ten real minutes apart
// stand hours apart in game time, one at noon and one after dark.
//
// Weather itself never travels. Selection is a pure function of (seed, game
// time) over a climate both machines parsed from the same records, so the seed
// plus the clock IS the weather, cross-fades and lightning strikes included.
// `weather` names the form in force on the host only as a correction: a host
// that resumed a savegame or was told to force a weather has a def in its
// climate the client's list may lack, and the same seed would then pick a
// different slot. The client aligns onto the named form when its own selection
// disagrees.
struct WorldState {
  f64 game_days = 0.0;    // WorldClock::game_days (whole part = days elapsed)
  f32 timescale = 20.0f;  // game seconds per real second (Bethesda default 20)
  u64 weather_seed = 0;   // weather::Director::seed
  u64 weather = 0;        // packed form of the weather in force, 0 = unknown
};

// Fixed 28-byte little-endian record:
//   f64 game_days | f32 timescale | u64 weather_seed | u64 weather
// (floats as their IEEE bit patterns, like every other codec here).
std::vector<u8> EncodeWorldState(const WorldState& state);

// Returns nullopt on a buffer of the wrong size, never reading out of bounds.
std::optional<WorldState> DecodeWorldState(const u8* data, size_t size);

}  // namespace rx::net

#endif  // RECREATION_NET_WORLD_STATE_H_
