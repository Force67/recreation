#ifndef RECREATION_BETHESDA_ACTOR_STATS_H_
#define RECREATION_BETHESDA_ACTOR_STATS_H_

// An actor's level and temperament, as the NPC_ record's ACBS and AIDT blocks
// carry them. A savegame rewrites the same two blocks (see savegame_changeform.h),
// so this is the one shape both the records and a resumed save resolve into.

#include "core/types.h"

namespace rx::bethesda {

struct Record;

// ACBS bit that turns the stored level into a 1000-based multiplier of the
// player's level rather than a level of its own.
constexpr u32 kActorBaseFlagLevelMult = 0x00000080;

// AIDT: how an actor picks fights, how long it holds them, and who it helps.
// The six values are one byte each, in this order, in the record and in a save
// alike, and Papyrus reads them back as the actor values of the same names.
struct ActorAi {
  u8 aggression = 0;  // 0 unaggressive, 1 aggressive, 2 very aggressive, 3 frenzied
  u8 confidence = 0;  // 0 cowardly, 1 cautious, 2 average, 3 brave, 4 foolhardy
  u8 energy = 0;      // how much the idle sandbox moves it about
  u8 morality = 0;    // 0 any crime, 1 violence against enemies, 2 property, 3 no crime
  u8 mood = 0;
  u8 assistance = 0;  // 0 helps nobody, 1 helps allies, 2 helps friends and allies
};

struct ActorStats {
  // Always a level, never the multiplier: ResolveActorLevel has already turned
  // a level-mult actor's multiplier into the level it stands at.
  u32 level = 1;
  bool has_ai = false;
  ActorAi ai;
};

// The level an actor is really at. A level-mult actor scales with the player and
// is then clamped to the range the record authors; a zero maximum is no cap. The
// scaling truncates, which is what puts a 1000 (1.0x) actor exactly at the
// player's level.
u32 ResolveActorLevel(u32 base_flags,
                      u32 stored_level,
                      u32 calc_min_level,
                      u32 calc_max_level,
                      u32 player_level);

// Reads ACBS + AIDT off an NPC_ record. False when the record carries no ACBS,
// which is the only part that is required; an actor with no AIDT comes back with
// has_ai clear rather than a profile of zeroes.
bool ReadActorStats(const Record& npc, u32 player_level, ActorStats* out);

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_ACTOR_STATS_H_
