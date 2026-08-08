#ifndef COMPONENTS_BETHESDA_SAVEGAME_CHANGEFORM_H
#define COMPONENTS_BETHESDA_SAVEGAME_CHANGEFORM_H

// Layer 2 of the savegame reader (see savegame.h): one ChangeForm payload in,
// one typed struct out. Pure - no world, no ECS, no allocator tricks; applying
// the result is layer 3's job.
//
// A payload is a flag-driven record. `ChangeForm::flags` says which optional
// field groups were written and the groups appear in a fixed order, so the only
// way through a payload is to walk the flags in that order; there are no offsets
// and no lengths to seek by. Get one group's size wrong and everything after it
// is garbage, which is why the decoders below stop and report how far they got
// instead of guessing past a group they do not know.
//
// Every layout here was derived from, and byte-exactly validated against, a
// Skyrim SE save (header version 12, form version 78, 151123 change forms):
// each decoder consumes its payload to the last byte for the flag combinations
// it claims to handle. Fields whose meaning is still unknown are named as such
// rather than guessed at.

#include <base/containers/span.h>
#include <base/containers/vector.h>

#include "components/bethesda/savegame.h"
#include "core/types.h"

namespace rx::bethesda {

// A form id embedded inside a payload: three bytes, big endian, the top two
// bits selecting how the remaining 22 bits resolve. Payload refs are always
// this, never a bare u32.
enum class ChangeRefKind : u8 {
  kFormIdIndex = 0,  // one-based index into SaveFile::form_ids, 0 means none
  kDefaultFile = 1,  // form id in the first master, i.e. 0x00xxxxxx
  kCreated = 2,      // form created at runtime, i.e. 0xFFxxxxxx
  kUnused = 3,
};

struct ChangeRef {
  ChangeRefKind kind = ChangeRefKind::kUnused;
  u32 value = 0;

  bool none() const {
    return kind == ChangeRefKind::kFormIdIndex && value == 0;
  }
};

// Returns 0 when the ref names nothing or indexes past the map, which is also
// how a malformed file shows up here, so callers must treat 0 as "no form".
u32 ResolveChangeRef(ChangeRef ref, base::Span<const u32> form_ids);

// --- REFR / ACHR -----------------------------------------------------------

constexpr u32 kRefrChangeFormFlags = 0x00000001;
constexpr u32 kRefrChangeMoved = 0x00000002;
constexpr u32 kRefrChangeHavokMoved = 0x00000004;
constexpr u32 kRefrChangeCellChanged = 0x00000008;
constexpr u32 kRefrChangeScale = 0x00000010;
constexpr u32 kRefrChangeInventory = 0x00000020;
constexpr u32 kRefrChangeLeveledInventory = 0x08000000;

// The runtime TESForm flags a reference carries, as written by the
// kRefrChangeFormFlags group.
constexpr u32 kFormFlagDeleted = 0x00000020;
constexpr u32 kFormFlagInitiallyDisabled = 0x00000800;

struct InventoryItem {
  ChangeRef item;
  i32 count = 0;
  // Number of extra-data blocks hanging off this stack (enchantment charge,
  // soul level, poison, ...). Their encoding is per-extra-type and is not
  // decoded, so a non-zero count ends the item walk.
  u32 extra_count = 0;
};

struct ReferenceChange {
  bool moved = false;
  ChangeRef parent;  // the cell, or the worldspace when the ref sits outside
  f32 position[3] = {};
  f32 rotation[3] = {};  // radians, x/y/z

  bool has_form_flags = false;
  u32 form_flags = 0;
  u16 form_flags_extra = 0;

  base::Vector<InventoryItem> inventory;
  // False when the walk stopped early on an item carrying extra data, i.e.
  // `inventory` is a prefix of the real contents.
  bool inventory_complete = false;

  // How much of the payload the decoder actually understood. Anything past this
  // is a group this layer does not decode, not corruption.
  mem_size decoded_bytes = 0;
};

// Handles both REFR and ACHR, but an ACHR yields the transform only: the actor
// record carries state the plain reference layout does not describe, so the
// walk stops rather than land mid-record on the groups after it.
bool DecodeReference(const ChangeForm& form, ReferenceChange& out);

// --- QUST ------------------------------------------------------------------

constexpr u32 kQuestChangeStages = 0x80000000;
constexpr u32 kQuestChangeObjectives = 0x20000000;
constexpr u32 kQuestChangeRunData = 0x10000000;
constexpr u32 kQuestChangeInstances = 0x08000000;

struct QuestStageState {
  u16 stage = 0;
  u8 flags = 0;  // bit 0 set once the stage has run
};

struct QuestObjectiveState {
  u32 index = 0;
  u32 state = 0;  // bit 0 displayed, bit 1 completed
};

struct QuestAliasFill {
  u32 alias_id = 0;
  ChangeRef ref;
};

struct QuestChange {
  u8 quest_flags = 0;
  u8 priority = 0;
  base::Vector<QuestStageState> stages;
  base::Vector<QuestObjectiveState> objectives;
  base::Vector<QuestAliasFill> alias_fills;
  mem_size decoded_bytes = 0;
};

bool DecodeQuest(const ChangeForm& form, QuestChange& out);

// --- NPC_ (actor base) -----------------------------------------------------

constexpr u32 kActorBaseChangeStats = 0x00000002;      // ACBS
constexpr u32 kActorBaseChangeAi = 0x00000008;         // AIDT
constexpr u32 kActorBaseChangeUnknown10 = 0x00000010;  // variable, not decoded
constexpr u32 kActorBaseChangeFactions = 0x00000040;   // SNAM
constexpr u32 kActorBaseChangeSkills = 0x00000200;     // DNAM

// ACBS bit that turns `level` into a 1000-based multiplier of the player level.
constexpr u32 kActorBaseFlagLevelMult = 0x00000080;

constexpr u32 kActorSkillCount = 18;

struct ActorFactionRank {
  ChangeRef faction;
  i8 rank = 0;
};

struct ActorBaseChange {
  bool has_stats = false;
  u32 base_flags = 0;
  u16 magicka_offset = 0;
  u16 stamina_offset = 0;
  u16 level = 0;  // level, or level*1000 multiplier when kActorBaseFlagLevelMult
  u16 calc_min_level = 0;
  u16 calc_max_level = 0;
  u16 speed_multiplier = 0;
  u16 disposition_base = 0;
  u16 template_flags = 0;
  i16 health_offset = 0;
  u16 bleedout_override = 0;

  base::Vector<ActorFactionRank> factions;

  bool has_ai = false;
  u8 aggression = 0;
  u8 confidence = 0;
  u8 energy = 0;
  u8 morality = 0;
  u8 mood = 0;
  u8 assistance = 0;

  bool has_skills = false;
  u8 skills[kActorSkillCount] = {};
  u8 skill_offsets[kActorSkillCount] = {};
  u16 health = 0;
  u16 magicka = 0;
  u16 stamina = 0;
  f32 far_away_model_distance = 0.0f;

  mem_size decoded_bytes = 0;
};

bool DecodeActorBase(const ChangeForm& form, ActorBaseChange& out);

// --- FACT ------------------------------------------------------------------

constexpr u32 kFactionChangeFormFlags = 0x00000002;
constexpr u32 kFactionChangeReactions = 0x00000004;
constexpr u32 kFactionChangeCrime = 0x80000000;

struct FactionReaction {
  ChangeRef faction;
  i32 modifier = 0;
  u32 combat_reaction = 0;  // 0 neutral, 1 enemy, 2 ally, 3 friend
};

struct FactionChange {
  base::Vector<FactionReaction> reactions;

  bool has_form_flags = false;
  u32 form_flags = 0;

  // Written for crime factions. The two counts track the player's outstanding
  // bounty; which is the violent one is not established, so both are exposed.
  bool has_crime = false;
  u32 crime_count_a = 0;
  u32 crime_count_b = 0;
  f32 crime_time_a = 0.0f;  // -FLT_MAX when never set
  f32 crime_time_b = 0.0f;

  mem_size decoded_bytes = 0;
};

bool DecodeFaction(const ChangeForm& form, FactionChange& out);

// --- INFO ------------------------------------------------------------------

constexpr u32 kInfoChangeSaid = 0x80000000;

// Dialogue state is carried entirely by the change flags; the payload is empty.
struct DialogueInfoChange {
  bool said = false;
};

bool DecodeDialogueInfo(const ChangeForm& form, DialogueInfoChange& out);

// --- CELL ------------------------------------------------------------------

constexpr u32 kCellChangeFormFlags = 0x00000002;
constexpr u32 kCellChangeDetached = 0x20000000;
constexpr u32 kCellChangeVisitedGrid = 0x40000000;
constexpr u32 kCellChangeVisited = 0x80000000;

// One 16x16 bit grid of the cell that the player has uncovered, as drawn on the
// world map, plus the row mask that goes with it.
struct CellVisitedGrid {
  u16 mask = 0;
  u8 bits[32] = {};
};

struct CellChange {
  bool detached = false;  // cell purged from memory, its refs unloaded

  bool has_form_flags = false;
  u16 form_flags = 0;

  base::Vector<CellVisitedGrid> visited;

  mem_size decoded_bytes = 0;
};

bool DecodeCell(const ChangeForm& form, CellChange& out);

}  // namespace rx::bethesda

#endif  // COMPONENTS_BETHESDA_SAVEGAME_CHANGEFORM_H
