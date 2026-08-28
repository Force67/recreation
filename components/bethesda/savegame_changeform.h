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
#include <base/strings/xstring.h>

#include "components/bethesda/actor_stats.h"
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

  // Whether the ref names no form. Both spellings of that count: a payload
  // writes "nothing" as index 0, and a field the decoder never reached keeps
  // kUnused, the kind the game never writes. Reading only the first spelling as
  // none made every unset field on a stack look like a poison, an enchantment
  // and an owner all at once.
  bool none() const {
    return kind == ChangeRefKind::kUnused ||
           (kind == ChangeRefKind::kFormIdIndex && value == 0);
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
constexpr u32 kRefrChangeBaseObject = 0x00000080;
constexpr u32 kRefrChangeLock = 0x00001000;
constexpr u32 kRefrChangeEmpty = 0x00200000;
constexpr u32 kRefrChangeOpenDefaultState = 0x00400000;
constexpr u32 kRefrChangeOpenState = 0x00800000;
constexpr u32 kRefrChangePromoted = 0x02000000;
constexpr u32 kRefrChangeActivatingChildren = 0x04000000;
constexpr u32 kRefrChangeLeveledInventory = 0x08000000;
constexpr u32 kRefrChangeAnimation = 0x10000000;

// The runtime TESForm flags a reference carries, as written by the
// kRefrChangeFormFlags group.
constexpr u32 kFormFlagDeleted = 0x00000020;
constexpr u32 kFormFlagInitiallyDisabled = 0x00000800;

// What a stack's nested extra-data lists say about the copies in it, beyond how
// many there are. A stack writes one list per copy that carries any, so two
// differently named swords in one stack write two lists; the last copy to carry
// a field wins here and `described_copies` says how many there were, which is
// as much as an inventory of (form, count) can hold anyway.
struct InventoryItem {
  ChangeRef item;
  i32 count = 0;
  // Set by the ExtraWorn / ExtraWornLeft markers on the stack, which is how a
  // save says an actor has the thing in hand rather than just in its bag.
  bool equipped = false;
  bool equipped_left = false;

  // How many copies of the stack wrote an extra-data list of their own.
  u32 described_copies = 0;

  // ExtraCount. The copies one list stands for, which is what makes a stack of
  // 6315 arrows one list rather than 6315. Agrees with `count` on 19605 of the
  // 24751 stacks that carry it; the rest are stacks split across several lists,
  // or the negative deltas of a looted container.
  bool has_stack_count = false;
  u16 stack_count = 0;

  // ExtraHealth, i.e. the temper. A multiplier on the item's authored health,
  // 1.0 being untempered; the reference save's 116 rows run 1.8 to 5.4, which is
  // the range Skyrim's smithing produces and never below untempered.
  bool has_health = false;
  f32 health = 1.0f;

  // ExtraEnchantment: the enchantment the player put on the item, and the charge
  // it holds when full. Every one of the reference save's 138 rows names a form
  // the save itself created, and every one of those is in the created-form table
  // as a weapon (26) or armour (112) enchantment, which is what says this field
  // is the enchantment and not some other ref.
  ChangeRef enchantment;
  u16 enchantment_charge = 0;

  // ExtraCharge: how much of that charge is left, as a float. Only a weapon
  // spends charge, which is why the reference save carries 44 of these against
  // 138 enchantments.
  bool has_charge = false;
  f32 charge = 0.0f;

  // ExtraSoul: the soul in a soul gem, 1 petty through 5 grand. Exactly those
  // five values across the reference save's 47 rows, all of them on soul gems.
  u8 soul = 0;

  // ExtraHotkey. The reference save writes 0xff (no slot) on all 24, which is
  // how Skyrim spells "favourited but not bound to a key", so the entry's
  // presence is the favourite and the byte is the slot.
  bool favourite = false;
  u8 hotkey = 0;

  // ExtraOwnership: who the copy belongs to, which is what makes it stolen when
  // that is not the player. An NPC or a faction; the save does not say which.
  ChangeRef owner;

  // ExtraPoison: what the player coated the weapon with, and how many uses are
  // left. Decoded from the same 7 bytes as the other refs but never exercised:
  // the reference save carries no poisoned stack at all.
  ChangeRef poison;
  u32 poison_doses = 0;

  // ExtraTextDisplayData, but only the spelling that carries a string: two empty
  // refs and -2, which is how a save records a name the player typed at an
  // enchanting or smithing table. The other spellings name a form that supplies
  // the text and are left alone.
  base::String name;
};

// One row of an actor's value table: a game ActorValue index (24 is Health, 6
// to 23 the eighteen skills) and what the save recorded for it.
struct ActorValueEntry {
  u32 index = 0;
  f32 value = 0.0f;
  u8 modifier = 0;  // which modifier slot the row belongs to; meaning unproven
};

// How many value tables sit together in an actor's payload, and the bounds a
// table has to respect to be one rather than a coincidence.
constexpr u32 kActorValueTableCount = 6;
constexpr u32 kActorValueIndexCount = 256;
constexpr u32 kMaxActorValueEntries = 256;

// One perk an actor's block records, with the byte that follows the ref. That
// byte reads 1 for every one of the 297 perks in the reference save, which is
// what Skyrim's own records author (a multi-rank perk is a chain of separate
// PERK records linked by NNAM, not one record at rank 3), so it is reported as
// read rather than interpreted as a rank.
struct ActorPerk {
  ChangeRef perk;
  u8 rank = 0;
};

// Bounds the perk search: a count past this, or a rank byte past that, is not a
// perk array. Skyrim plus Update and the three add-ons author 523 PERK records
// in total, so no actor of that load order can hold more than that many.
constexpr u32 kMaxActorPerks = 2048;
constexpr u8 kMaxActorPerkRank = 8;

// Bounds the added-spell search (see FindActorSpells). The minimum is where the
// shape stops being a coincidence, and it was measured rather than picked: swept
// over the 20658 actor blocks of the reference save, a minimum of 12 leaves 65
// blocks with a single reading and 64 of those read arrays of references, not
// spells; 24 leaves 5, still all references; 32 leaves exactly one in the whole
// file, the player's, and all 161 of its entries resolve to SPEL records. An
// actor with fewer added spells than that is not reported at all.
constexpr u32 kMinActorSpells = 32;
constexpr u32 kMaxActorSpells = 2048;

// The flags a map marker's extra data carries. Discovering a location sets both
// of the first two: visible puts the icon on the map, can-travel makes it a fast
// travel destination. The third is the "show all hidden markers" cheat.
constexpr u8 kMapMarkerVisible = 0x01;
constexpr u8 kMapMarkerCanTravelTo = 0x02;
constexpr u8 kMapMarkerShowAllHidden = 0x04;

// The only bit of a lock's flag byte the save sets that the record does not, so
// it is the one that says whether the thing is locked right now. Measured: the
// byte is the reference's own XLOC flag byte verbatim on all 1226 locks whose
// record authors one, plus this bit on 490 of them.
constexpr u8 kLockFlagLocked = 0x01;

struct ReferenceChange {
  bool moved = false;
  ChangeRef parent;  // the cell, or the worldspace when the ref sits outside
  f32 position[3] = {};
  f32 rotation[3] = {};  // radians, x/y/z

  bool has_form_flags = false;
  u32 form_flags = 0;
  u16 form_flags_extra = 0;

  bool has_scale = false;
  f32 scale = 1.0f;
  ChangeRef base_object;  // only written for a reference that swapped its base

  base::Vector<InventoryItem> inventory;
  // False when the walk stopped before the end of the item list, i.e.
  // `inventory` is a prefix of the real contents.
  bool inventory_complete = false;

  // An ACHR's actor values, when the payload admitted exactly one reading of
  // them (see DecodeReference). Empty is "not found", never "the actor has
  // none".
  base::Vector<ActorValueEntry> actor_values;

  // An ACHR's perks, on the same terms as actor_values: found by a search of the
  // block rather than a walk to it, and reported only when the block admits
  // exactly one reading (see FindActorPerks). Empty is "not found".
  base::Vector<ActorPerk> perks;

  // The spells the actor learned during play, found the same way (see
  // FindActorSpells). This is where the player's real spell book lives: the NPC_
  // change form's spell list only holds the handful the game hands out early,
  // three in the reference save against the 161 here.
  base::Vector<ChangeRef> spells;

  // Set when the reference's extra-data list carries a map marker, i.e. this
  // REFR is a location on the world map and the save has an opinion about it.
  // Only the flags travel in the save; the marker's name, icon and position
  // stay in the REFR record the way the plugin authored them.
  bool has_map_marker = false;
  u8 map_marker_flags = 0;

  // The reference's lock, out of its extra-data list. Only a reference whose
  // lock the player changed carries one, which is why an unlocked door has to
  // be applied rather than left to the record.
  bool has_lock = false;
  u8 lock_level = 0;   // 0, 1, 5, 25, 50, 75, 100 or 255 ("needs the key")
  u8 lock_flags = 0;   // kLockFlagLocked, over the record's own XLOC flags
  ChangeRef lock_key;  // the KEYM that opens it, empty when none

  // Three states a container, door or activator carries in the change flags
  // alone: the payload has no room for them and none is written. `emptied` is
  // the only record that a container was cleared out, because a reference
  // carrying it writes no inventory group at all (measured on all 3433 of
  // them), so without it a looted chest comes back with everything the record
  // authors. `open_default` is decoded on the same terms as `open` but the
  // reference save never sets it, so it is unverified.
  bool emptied = false;
  bool open = false;
  bool open_default = false;

  // How much of the payload the decoder actually understood. Anything past this
  // is a group this layer does not decode, not corruption.
  mem_size decoded_bytes = 0;
};

// Handles both REFR and ACHR.
//
// The flag-driven groups (transform, form flags, base object, scale, extra
// data, inventory, animation) are walked in full for both. What follows them on
// an ACHR is the actor's own block: its AI process state first, which is
// variable length and not decoded here, and the actor value tables and perk
// arrays inside it. Neither can be walked to, so both are searched for and only
// reported when the payload admits exactly one reading; see the file for the
// two rules.
//
// WHETHER AN ACTOR IS DEAD IS NOT IN REACH HERE, and the search for it is worth
// writing down so it is not repeated. Every ACHR change flag the reference save
// uses is accounted for by a group or an extra-data type except 0x00000400,
// which 6489 of its 20658 actors carry. That bit is not the life state: the
// player's own ACHR sets it, and resolving the actors that carry it through
// Skyrim.esm puts Kodlak Whitemane, Grelod and Mercer Frey (dead in any
// finished game) beside Balgruuf, Aela and Farkas (alive in one). Nor does it
// introduce a field: for every group boundary in the walk above and every width
// from one to eight bytes, inserting a group for that bit leaves the actor
// block's opening byte less like the actors that do not carry it, not more.
// What the save does carry is the actor's own value table, and health there is
// the state the runtime reads: 67 actors have one and two of those are at or
// below zero health, so those two load dead and the rest of a save's corpses
// stand up. Cracking that needs the AI process block decoded, not another flag.
// variable length and not decoded here, and the actor value tables, perk arrays
// and added-spell list inside it. None of the three can be walked to, so all
// three are searched for and only reported when the payload admits exactly one
// reading; see the file for the rules.
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

constexpr u32 kActorBaseChangeFormFlags = 0x00000001;
constexpr u32 kActorBaseChangeStats = 0x00000002;     // ACBS
constexpr u32 kActorBaseChangeAi = 0x00000008;        // AIDT
constexpr u32 kActorBaseChangeSpells = 0x00000010;    // spells and shouts
constexpr u32 kActorBaseChangeFullName = 0x00000020;  // FULL
constexpr u32 kActorBaseChangeFactions = 0x00000040;  // SNAM
constexpr u32 kActorBaseChangeSkills = 0x00000200;    // DNAM
constexpr u32 kActorBaseChangeClass = 0x00000400;     // CNAM
constexpr u32 kActorBaseChangeFace = 0x00000800;      // the whole FaceGen block
constexpr u32 kActorBaseChangeDefaultOutfit = 0x00001000;  // DOFT
constexpr u32 kActorBaseChangeSleepOutfit = 0x00002000;    // SOFT
constexpr u32 kActorBaseChangeGender = 0x01000000;         // ACBS sex bit
constexpr u32 kActorBaseChangeRace = 0x02000000;           // RNAM

// `level` becomes a 1000-based multiplier of the player level under
// kActorBaseFlagLevelMult (actor_stats.h), the same bit the NPC_ record uses.

constexpr u32 kActorSkillCount = 18;

struct ActorFactionRank {
  ChangeRef faction;
  i8 rank = 0;
};

// What chargen left on an actor's head: the same fields the NPC_ record's
// FaceGen block authors (HCLF, QNAM, FTST, PNAM, NAM9, NAMA), written back
// because the player changed them. The record's own copy is the authored face,
// so this group is the only description of the face the save actually plays.
struct ActorFaceChange {
  ChangeRef hair_color;    // CLFM
  ChangeRef face_texture;  // TXST, the baked face texture set
  // The QNAM skin tint, packed. Read as 0x00RRGGBB rather than a form: the
  // reference save's 0x002A3540 matches no record in its load order, and the
  // three low bytes are the byte triple a CLFM's CNAM stores. One save carries
  // this group, so the packing is the best reading rather than a proven one.
  u32 skin_tone = 0;
  base::Vector<ChangeRef> head_parts;  // HDPT, one per slot the face fills
  bool has_morphs = false;
  base::Vector<f32> morphs;   // NAM9 sliders, 19 of them
  base::Vector<i32> presets;  // NAMA face-part indices, -1 for none
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

  // What the actor knows: the spells it was given, the levelled variants the
  // game rolled for it, and one entry per shout it has words for.
  base::Vector<ChangeRef> spells;
  base::Vector<ChangeRef> levelled_spells;
  base::Vector<ChangeRef> shouts;

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

  // The name the actor answers to now: what chargen typed for the player, or
  // what a script renamed an NPC to.
  bool has_full_name = false;
  base::String full_name;

  // The race the actor is now, then the one its record authors. That order is
  // measured, not assumed: read this way all six actors that carry the group in
  // the reference save land on RACE records and say the right thing (the player
  // ArgonianRace over NordRace, Sinding and a Dawnguard farmer WerewolfBeastRace,
  // Harkon DLC1VampireBeastRace, two Dragonborn tribals DLC2WerebearBeastRace,
  // every one of them over the NordRace their record authors). Read the other way
  // round, or with the group anywhere else in the payload, none of the six
  // resolves to a RACE at all.
  bool has_race = false;
  ChangeRef race;
  ChangeRef original_race;

  // The reference save never writes this group (no actor in it changed sex), so
  // the one byte is inherited from the levelled-actor walk rather than measured.
  bool has_gender = false;
  bool female = false;

  bool has_face = false;
  ActorFaceChange face;

  mem_size decoded_bytes = 0;
};

// The groups come in the order the constants are listed above except that the
// name sits between the AI data and the skills, and the race between the skills
// and the face. Verified by consuming all 926 NPC_ payloads of the reference
// save to their last byte, which pins every group's size; where the byte count
// alone leaves a group's position open (four orderings of the player's payload
// balance), what settles it is that only one of them resolves the race refs to
// RACE records. The four groups the reference save never writes (form flags,
// class, sleep outfit, gender) are unverified.
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

  // Written for crime factions. The two counts are the player's infamy with the
  // faction, i.e. how many crimes of each kind it has seen, which is not the
  // same thing as the outstanding bounty and is never cleared by paying one.
  //
  // Established against the reference save rather than assumed. Neither count
  // can be gold, outstanding or accumulated: CrimeFactionImperial and
  // CrimeFactionThievesGuild author a CRVA of all zeroes, so no crime against
  // them is ever worth any gold, yet they read 73 and 18 here; the save's own
  // misc stats put every hold's Bounty at 0 and the lifetime bounty at 83539,
  // neither of which is any of these numbers. Violent is the first of the two:
  // summed over all 72 crime factions the first count is 541 against a lifetime
  // 169 murders + 361 assaults (the excess is one crime seen by more than one
  // faction), while the second sums to 1968, far past anything violent, and is
  // non-zero exactly where the player stole rather than killed (ThievesGuild
  // 0/18, Imperial 73/0, CompanionsFaction 1/47).
  bool has_crime = false;
  u32 infamy_violent = 0;
  u32 infamy_non_violent = 0;
  // Two game-time stamps in hours. The first reads -FLT_MAX ("never") in every
  // record of the reference save and the second is usually a real hour, so what
  // each one times is not established.
  f32 crime_time_a = 0.0f;
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

// --- WOOP ------------------------------------------------------------------

constexpr u32 kWordOfPowerChangeFormFlags = 0x00000001;

// The runtime form flag a word carries once the player has it. Measured, not
// looked up: every one of the 84 words the reference save writes carries the
// same six byte form-flag group, 0x00010049 and a zero u16, while the 54 ENCH
// change forms write that same group as 0x00000049. So 0x49 is what any touched
// form of this save carries and 0x00010000 is the bit that is the word's own.
constexpr u32 kWordOfPowerKnown = 0x00010000;

// A word of power the player learned off a word wall and unlocked with a dragon
// soul. Which of the two states the bit above stands for cannot be told apart
// here: a 100% save has done both to every word it lists, so the two are only
// ever seen set together.
struct WordOfPowerChange {
  bool known = false;
  mem_size decoded_bytes = 0;
};

bool DecodeWordOfPower(const ChangeForm& form, WordOfPowerChange& out);

// --- CELL ------------------------------------------------------------------

constexpr u32 kCellChangeFormFlags = 0x00000002;
constexpr u32 kCellChangeOwnership = 0x00000008;
constexpr u32 kCellChangeExteriorGrid = 0x20000000;
constexpr u32 kCellChangeDetachTime = 0x40000000;
constexpr u32 kCellChangeVisited = 0x80000000;

// One 16x16 bit grid the player has uncovered. An exterior cell is one tile of
// the world map and writes only the bits; an interior's local map is a set of
// tiles, each carrying its own coordinate in the interior's own grid.
struct CellVisitedGrid {
  bool has_tile = false;
  i8 tile_x = 0;
  i8 tile_y = 0;
  u8 bits[32] = {};
};

struct CellChange {
  // The cell's own grid coordinate, written by the save rather than read off
  // the record, and the one thing that says a CELL change form is an exterior:
  // every interior in the reference save writes none. Measured, not assumed:
  // all 8148 that carry it match the coordinate the CELL record's XCLC gives.
  bool has_grid = false;
  i8 grid_x = 0;
  i8 grid_y = 0;
  // Constant per worldspace across the reference save (Tamriel reads 1), so it
  // indexes the worldspace somehow; which table is not established.
  u16 grid_world = 0;

  // The game hour the cell was last taken out of memory, which is what a cell
  // reset counts from. Not an exceptional state: every cell but the handful
  // still in memory around the player carries one.
  bool has_detach_time = false;
  u32 detach_time = 0;

  bool has_form_flags = false;
  u16 form_flags = 0;

  base::Vector<CellVisitedGrid> visited;

  mem_size decoded_bytes = 0;
};

bool DecodeCell(const ChangeForm& form, CellChange& out);

// --- LCTN ------------------------------------------------------------------

constexpr u32 kLocationChangeCleared = 0x80000000;
constexpr u32 kLocationChangeKeywordData = 0x40000000;

// One (keyword, number) pair the runtime hung on a location. Skyrim's own
// SetKeywordData / GetKeywordDataForLocation read this table, which is why the
// keyword need not be one the location's record lists.
struct LocationKeywordValue {
  ChangeRef keyword;
  f32 value = 0.0f;
};

struct LocationChange {
  // Whether the player has finished the place off, i.e. what the map means by
  // "Cleared". Measured: of the 374 LCTN change forms in the reference save 248
  // carry this group and 48 of those say cleared, and the four that say so
  // without their record carrying the LocTypeClearable keyword are exactly the
  // four Skyrim's own scripts force-clear (Helgen, Goldenglow Estate,
  // Honningbrew Meadery, the Arch-Mage's Quarters), which is what pins this byte
  // down as the cleared flag rather than something correlated with it.
  bool has_cleared = false;
  bool cleared = false;
  // A second byte in the same group. It reads 1 in every one of the 248 records
  // that carry the group, so what it says cannot be established from one save;
  // it is kept only so nothing silently drops a byte.
  u8 cleared_extra = 0;

  base::Vector<LocationKeywordValue> keyword_data;

  mem_size decoded_bytes = 0;
};

bool DecodeLocation(const ChangeForm& form, LocationChange& out);

// --- ECZN ------------------------------------------------------------------

constexpr u32 kEncounterZoneChangeGameData = 0x80000000;

struct EncounterZoneChange {
  bool has_game_data = false;
  // The level the zone locked to the first time the player walked into it, and
  // the whole point of reading this record: without it every dungeon rescales
  // to the player's current level. Measured against the records: all 233 zones
  // of the reference save sit at or above the min level their ECZN authors (8
  // exactly at it) and inside its max, and the levels span 6 (Bleak Falls
  // Barrow, the first dungeon) to 271 (the player's own level).
  u32 level = 0;
  // Three counters ahead of the level, ordered a >= b >= c in all 233 records
  // and all inside the save's 62784 elapsed game hours. That is consistent with
  // game-hour stamps of the zone's last visit, but which moment each one marks
  // is not established, so they are reported rather than interpreted.
  u32 stamp_a = 0;
  u32 stamp_b = 0;
  u32 stamp_c = 0;

  mem_size decoded_bytes = 0;
};

bool DecodeEncounterZone(const ChangeForm& form, EncounterZoneChange& out);

// --- LVLI / LVLN -----------------------------------------------------------

constexpr u32 kLeveledListChangeAddedObject = 0x80000000;

struct LeveledListEntry {
  ChangeRef form;
  u16 level = 0;
  u16 count = 0;
};

struct LeveledListChange {
  // Entries a script put on the list at runtime, on top of the LVLO entries the
  // record authors. Nothing removes them, so this is purely additive.
  base::Vector<LeveledListEntry> added;
  mem_size decoded_bytes = 0;
};

// Handles both LVLI (loot) and LVLN (actors); the two share the payload.
bool DecodeLeveledList(const ChangeForm& form, LeveledListChange& out);

// --- BOOK ------------------------------------------------------------------

// Carries the book's runtime DATA flags byte.
constexpr u32 kBookChangeRead = 0x00000040;
// Set on exactly the 90 skill books of the reference save, which is also what
// that save's own "Books Read" misc stat counts, so it marks a skill book whose
// skill the player has already taken.
constexpr u32 kBookChangeSkillTaken = 0x00000020;

// The BOOK record's DATA flags, as the runtime carries them.
constexpr u8 kBookFlagTeachesSkill = 0x01;
constexpr u8 kBookFlagCannotBeTaken = 0x02;
constexpr u8 kBookFlagTeachesSpell = 0x04;
// Never authored by a record and set in all 766 of the reference save's book
// change forms, so this is the bit the act of reading sets.
constexpr u8 kBookFlagRead = 0x08;

struct BookChange {
  bool has_flags = false;
  // The whole runtime flags byte, which is the authored one plus the read bit,
  // minus the teaches-skill bit once the skill has been taken. Measured over
  // all 766: authored 0x00 -> 0x08 (x559), 0x02 -> 0x0a (x10), 0x04 -> 0x0c
  // (x107) and 0x01 -> 0x08 (x90), i.e. only the skill books lose a bit.
  u8 flags = 0;
  bool read = false;
  bool skill_taken = false;

  mem_size decoded_bytes = 0;
};

bool DecodeBook(const ChangeForm& form, BookChange& out);

// --- INGR ------------------------------------------------------------------

constexpr u32 kIngredientChangeUse = 0x80000000;

// An ingredient has four effects and the payload is one u32, which reads 0xF in
// all 108 records of the reference save; all 108 name an INGR with exactly four
// effects, and 15 is not a count of four, so it is a bit per effect slot. Which
// bit is which slot cannot be shown by a save where every ingredient is fully
// known, so bit N naming the Nth EFID is inferred from the width, not measured.
constexpr u32 kIngredientAllEffectsKnown = 0x0000000F;

struct IngredientChange {
  bool has_known_effects = false;
  u32 known_effects = 0;
  mem_size decoded_bytes = 0;
};

bool DecodeIngredient(const ChangeForm& form, IngredientChange& out);

// --- RELA ------------------------------------------------------------------

constexpr u32 kRelationshipChangeData = 0x00000002;

// The rank as a RELA record spells it, which counts the other way from the one
// Papyrus reports: 0 is the closest bond and 8 the deepest hatred. Established
// against the reference save's one authored RELA, CamillaValeriusRaevild, whose
// record authors rank 3 and whose change form reads 5: the Riverwood love
// triangle turns a friendship into a rivalry, which is 3 -> 5 in this direction
// and would be a nonsensical -1 -> 1 in the other.
constexpr u32 kRelationshipRankLover = 0;
constexpr u32 kRelationshipRankAcquaintance = 4;
constexpr u32 kRelationshipRankArchnemesis = 8;

struct RelationshipChange {
  // A relationship the save invented rather than one a plugin authored. Its id
  // is 0xFFxxxxxx, so no record describes the pair and the payload carries it:
  // 263 of the reference save's 264 relationships are of this kind, and every
  // one of them names the player's base form on one side.
  bool created = false;
  ChangeRef parent;
  ChangeRef child;
  // The RELA record's association type. Null in all 263 created relationships
  // of the reference save, so its position in the payload rests on the record's
  // own field order rather than on a non-null example.
  ChangeRef association;

  u32 rank = 0;
  mem_size decoded_bytes = 0;
};

bool DecodeRelationship(const ChangeForm& form, RelationshipChange& out);

}  // namespace rx::bethesda

#endif  // COMPONENTS_BETHESDA_SAVEGAME_CHANGEFORM_H
