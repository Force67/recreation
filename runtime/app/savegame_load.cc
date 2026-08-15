#include <chrono>
#include <cmath>
#include <fstream>

#include <base/algorithm.h>
#include <base/containers/unordered_map.h>
#include <base/containers/unordered_set.h>
#include <base/containers/vector.h>
#include <base/memory/move.h>
#include <base/memory/unique_pointer.h>
#include <base/option.h>

#include "components/bethesda/record.h"
#include "components/bethesda/savegame_apply.h"
#include "components/quest/quest_system.h"
#include "components/script/games/skyrim/skyrim_bindings.h"
#include "components/script/games/skyrim/skyrim_native_state.h"
#include "components/script/papyrus/alias_handle.h"
#include "components/script/papyrus/value.h"
#include "components/weather/weather_loader.h"
#include "components/world/cell_streaming.h"
#include "core/log.h"
#include "runtime/actor/actor_system.h"
#include "runtime/app/engine.h"
#include "runtime/app/engine_internal.h"
#include "runtime/character/face.h"

// Booting from a savegame: read the file, remap its form ids onto this run's
// load order, push what the engine has a home for at the live systems, and
// resume the player where the save left them.
//
// The three entry points run at the three moments the data they need exists:
// LoadSavegame while the LoadOrder is still in scope, ApplySavegameState once
// the bindings are up but before quest scripts attach (so nothing replays), and
// ApplySavegameLocation / PlaceSavegamePlayer around the cell streamer.
namespace rx {
namespace {

// Boot from a save without a command line, the same way --interior has an env
// twin. --load-save wins when both are given.
static base::Option<const char*> LoadSaveFile{"load.save", nullptr, "RX_LOAD_SAVE",
                                              "boot from this savegame file"};

constexpr u32 kEdid = FourCc('E', 'D', 'I', 'D');
constexpr u32 kWrld = FourCc('W', 'R', 'L', 'D');
constexpr u32 kCell = FourCc('C', 'E', 'L', 'L');
constexpr u32 kAchr = FourCc('A', 'C', 'H', 'R');
constexpr u32 kName = FourCc('N', 'A', 'M', 'E');  // a reference's base form
constexpr u32 kData = FourCc('D', 'A', 'T', 'A');  // a reference's placement
constexpr u32 kXscl = FourCc('X', 'S', 'C', 'L');  // a reference's scale
constexpr u32 kMgef = FourCc('M', 'G', 'E', 'F');
constexpr u32 kFull = FourCc('F', 'U', 'L', 'L');  // a record's displayed name
constexpr u32 kPerk = FourCc('P', 'E', 'R', 'K');
constexpr u32 kSpel = FourCc('S', 'P', 'E', 'L');
constexpr u32 kShou = FourCc('S', 'H', 'O', 'U');
constexpr u32 kWoop = FourCc('W', 'O', 'O', 'P');
constexpr u32 kRace = FourCc('R', 'A', 'C', 'E');
constexpr u32 kLctn = FourCc('L', 'C', 'T', 'N');
// The map marker reference a location draws itself at.
constexpr u32 kMnam = FourCc('M', 'N', 'A', 'M');
// The TESForm flag a record carries when it is placed disabled. The engine
// reads the same bit off the record when it streams a reference in.
constexpr u32 kRecordFlagInitiallyDisabled = 0x800;
// The NPC_ record behind the player reference, "Player" in both games this
// reader covers. The save writes the player's level and skills onto it.
constexpr u32 kPlayerBaseFormId = 0x00000007;
// Gold001, the currency record every Skyrim inventory counts its money in.
constexpr u32 kGoldFormId = 0x0000000f;

bool GameMatchesSave(bethesda::Game game, bethesda::SaveFormat format) {
  switch (format) {
    case bethesda::SaveFormat::kSkyrimLe:
    case bethesda::SaveFormat::kSkyrimSe:
      return game == bethesda::Game::kSkyrimSe;
    case bethesda::SaveFormat::kFallout4:
      return game == bethesda::Game::kFallout4;
    default:
      return false;
  }
}

bool ReadWholeFile(const base::String& path, base::Vector<u8>* out) {
  std::ifstream file(path.c_str(), std::ios::binary | std::ios::ate);
  if (!file)
    return false;
  const std::streamsize size = file.tellg();
  if (size <= 0)
    return false;
  file.seekg(0);
  out->resize(static_cast<size_t>(size));
  return bool(file.read(reinterpret_cast<char*>(out->data()), size));
}

// What the running game believes the player is, once the save has been applied.
// Read back out of the live systems rather than kept from the decoder, so the
// report says what the game will actually play with.
struct PlayerReadout {
  i32 level = 0;
  f32 health = 0, magicka = 0, stamina = 0, carry_weight = 0;
  f32 one_handed = 0, sneak = 0, destruction = 0;
  i32 perks = 0;
  i32 gold = 0;
  i32 spells = 0, shouts = 0, words = 0, favourites = 0;
  base::String name;
};

PlayerReadout ReadPlayerBack(script::skyrim::RecordBackedSkyrimBindings& bindings) {
  const script::papyrus::ObjectRef player{
      bethesda::GlobalFormId{0, bethesda::kPlayerFormId}.packed()};
  PlayerReadout out;
  out.level = bindings.GetLevel(player);
  out.health = bindings.GetBaseActorValue(player, "Health");
  out.magicka = bindings.GetBaseActorValue(player, "Magicka");
  out.stamina = bindings.GetBaseActorValue(player, "Stamina");
  out.carry_weight = bindings.GetBaseActorValue(player, "CarryWeight");
  out.one_handed = bindings.GetBaseActorValue(player, "OneHanded");
  out.sneak = bindings.GetBaseActorValue(player, "Sneak");
  out.destruction = bindings.GetBaseActorValue(player, "Destruction");
  out.perks = bindings.GetPerkCount(player);
  out.spells = bindings.GetSpellCount(player);
  out.shouts = bindings.GetShoutCount(player);
  out.words = bindings.GetKnownWordCount();
  out.favourites = bindings.GetMagicFavouriteCount();
  out.name = bindings.GetName(player);
  out.gold = bindings.GetItemCount(
      player, script::papyrus::ObjectRef{bethesda::GlobalFormId{0, kGoldFormId}.packed()});
  return out;
}

// Applies onto the live game. Runs on the Papyrus guest thread, which owns
// every store it writes to.
class EngineSaveSink : public bethesda::SaveSink {
 public:
  EngineSaveSink(script::skyrim::RecordBackedSkyrimBindings& bindings,
                 const bethesda::RecordStore& records,
                 world::ActorStatsStore& actor_stats,
                 dialogue::SaidTopics& said,
                 world::MapDiscovery& map,
                 world::MapMarkers& markers,
                 world::SavedSpawnIndex& spawns,
                 world::MiscStats& misc_stats,
                 world::CreatedForms& created_forms,
                 const bethesda::StringTable& strings,
                 FaceBuilder& faces,
                 f32 cell_size)
      : bindings_(bindings),
        records_(records),
        actor_stats_(actor_stats),
        said_(said),
        map_(map),
        markers_(markers),
        spawns_(spawns),
        misc_stats_(misc_stats),
        created_forms_(created_forms),
        strings_(strings),
        faces_(faces),
        cell_size_(cell_size) {}

  void SetGlobal(bethesda::GlobalFormId global, f32 value) override {
    bindings_.SetGlobalValue(Ref(global), value);
  }

  void SetMiscStat(base::StringRef name, u8 category, u32 value) override {
    misc_stats_.Set(name, category, value);
  }

  // Kept, not pushed: the sky is resumed after this whole pass, because that
  // needs the world clock and the clock only reads right once the globals above
  // have landed on it. The climate and the weather being faded out of are not
  // taken -- this engine's sky is driven by a climate it builds itself, and it
  // cross-fades on its own schedule.
  void SetWeather(bethesda::GlobalFormId,
                  bethesda::GlobalFormId weather,
                  bethesda::GlobalFormId,
                  f32) override {
    weather_ = weather;
  }

  bethesda::GlobalFormId weather() const { return weather_; }

  // Straight at the quest system, not through the bindings: the binding-level
  // SetStage runs the stage's script fragment, which would replay everything
  // the save has already been through.
  void SetQuestStageDone(bethesda::GlobalFormId quest, i32 stage) override {
    bindings_.quest_system().SetStage(quest.packed(), stage);
  }

  void SetQuestState(bethesda::GlobalFormId quest,
                     i32 stage,
                     bool running,
                     bool complete) override {
    quest::QuestStatus status;
    status.handle = quest.packed();
    status.stage = stage;
    status.running = running;
    status.complete = complete;
    bindings_.quest_system().ApplyStatus(status);
  }

  void SetQuestObjective(bethesda::GlobalFormId quest,
                         i32 objective,
                         bool displayed,
                         bool completed) override {
    bindings_.quest_system().SetObjectiveDisplayed(quest.packed(), objective, displayed);
    bindings_.quest_system().SetObjectiveCompleted(quest.packed(), objective, completed);
  }

  void FillQuestAlias(bethesda::GlobalFormId quest,
                      u32 alias_id,
                      bethesda::GlobalFormId ref) override {
    bindings_.AliasForceRefTo(
        script::papyrus::ObjectRef{script::papyrus::EncodeAliasHandle(quest.packed(), alias_id)},
        Ref(ref));
  }

  void SetActorValue(bethesda::GlobalFormId actor_base, base::StringRef name, f32 value) override {
    const u64 actor = PlacedActor(actor_base);
    if (actor == 0)
      return;
    bindings_.SetActorValue(script::papyrus::ObjectRef{actor},
                            base::String(name.data(), name.size()), value);
    ++actor_values_;
  }

  // Straight onto the reference: these are the values that actor levelled its
  // way to, so there is no base form to resolve and nothing to share.
  void SetReferenceActorValue(bethesda::GlobalFormId ref,
                              base::StringRef name,
                              f32 value) override {
    bindings_.SetActorValue(Ref(ref), base::String(name.data(), name.size()), value);
    ++reference_values_;
    if (ref.plugin == 0 && ref.local_id == bethesda::kPlayerFormId)
      ++player_values_;
  }

  void AddReferencePerk(bethesda::GlobalFormId ref,
                        bethesda::GlobalFormId perk,
                        u32 rank) override {
    // Only the ones this load order actually has a PERK record for: the remap
    // answers for any form the plugin holds, so a perk id that lands on some
    // other record type would be a perk the player never had.
    const bethesda::RecordStore::StoredRecord* stored = records_.Find(perk);
    if (!stored || stored->header.type != kPerk) {
      ++unknown_perks_;
      return;
    }
    bindings_.AddPerk(Ref(ref), Ref(perk), static_cast<i32>(rank));
    ++perks_;
  }

  // Spells and shouts land on the placed reference, not the base form: that is
  // what the bindings key an actor's knowledge by, and it is what a script's
  // HasSpell(Game.GetPlayer(), ...) asks about.
  void AddActorSpell(bethesda::GlobalFormId actor_base, bethesda::GlobalFormId spell) override {
    const u64 actor = PlacedActor(actor_base);
    if (actor == 0 || !IsRecord(spell, kSpel))
      return;
    bindings_.AddSpell(script::papyrus::ObjectRef{actor}, Ref(spell));
    ++spells_;
  }

  void AddActorShout(bethesda::GlobalFormId actor_base, bethesda::GlobalFormId shout) override {
    const u64 actor = PlacedActor(actor_base);
    if (actor == 0 || !IsRecord(shout, kShou))
      return;
    bindings_.AddShout(script::papyrus::ObjectRef{actor}, Ref(shout));
    ++shouts_;
  }

  void AddReferenceSpell(bethesda::GlobalFormId ref, bethesda::GlobalFormId spell) override {
    if (!IsRecord(spell, kSpel)) {
      ++unknown_spells_;
      return;
    }
    bindings_.AddSpell(Ref(ref), Ref(spell));
    ++spells_;
  }

  void SetWordOfPowerKnown(bethesda::GlobalFormId word) override {
    if (!IsRecord(word, kWoop)) {
      ++unknown_words_;
      return;
    }
    bindings_.UnlockWord(Ref(word));
    ++words_;
  }

  void AddMagicFavourite(bethesda::GlobalFormId form, i32 hotkey) override {
    bindings_.AddMagicFavourite(Ref(form), hotkey);
    ++favourites_;
  }

  void SetLastUsedMagic(bethesda::GlobalFormId weapon,
                        bethesda::GlobalFormId spell,
                        bethesda::GlobalFormId shout) override {
    bindings_.SetLastUsedMagic(Ref(weapon), Ref(spell), Ref(shout));
    last_shout_ = shout;
  }

  // Identity and appearance. The name goes onto the base form the way the save
  // stores it; the face and race go to the head builder, which is what turns
  // them back into a head when the player's body is assembled.
  void SetActorName(bethesda::GlobalFormId actor_base, base::StringRef name) override {
    const base::String text(name.data(), name.size());
    // Onto the base form, which is where the save records it and where a
    // reference's own GetName falls through to, and onto the placed reference,
    // whose id is what a script holds.
    bindings_.SetName(Ref(actor_base), text);
    const u64 actor = PlacedActor(actor_base);
    if (actor != 0)
      bindings_.SetName(script::papyrus::ObjectRef{actor}, text);
    ++names_;
  }

  // The original race is what a beast-form actor turns back into; nothing in the
  // engine transforms anyone yet, so only the race it is in now is applied.
  void SetActorRace(bethesda::GlobalFormId actor_base,
                    bethesda::GlobalFormId race,
                    bethesda::GlobalFormId) override {
    if (!IsRecord(race, kRace))
      return;
    faces_.OverrideRace(actor_base, race);
    ++races_;
  }

  void SetActorSex(bethesda::GlobalFormId actor_base, bool female) override {
    faces_.OverrideSex(actor_base, female);
  }

  void SetActorFace(bethesda::GlobalFormId actor_base, const bethesda::SavedFace& face) override {
    faces_.OverrideFace(actor_base, face);
    ++faces_applied_;
  }

  void SetActorFactionRank(bethesda::GlobalFormId actor_base,
                           bethesda::GlobalFormId faction,
                           i32 rank) override {
    const u64 actor = PlacedActor(actor_base);
    if (actor == 0)
      return;
    bindings_.SetFactionRank(script::papyrus::ObjectRef{actor}, Ref(faction), rank);
    ++faction_ranks_;
  }

  // Level and temperament land in two places on purpose: the bindings answer
  // Papyrus for the placed actor now, and the stats store keeps them for the
  // streamer to stamp onto the ECS actor whenever its cell comes in, which is
  // long after this runs.
  void SetActorLevel(bethesda::GlobalFormId actor_base, u32 level) override {
    actor_stats_.OverrideLevel(actor_base, level);
    const u64 actor = PlacedActor(actor_base);
    if (actor == 0)
      return;
    bindings_.SetLevel(script::papyrus::ObjectRef{actor}, static_cast<i32>(level));
    ++actor_levels_;
  }

  void SetActorAi(bethesda::GlobalFormId actor_base, const bethesda::ActorAi& ai) override {
    actor_stats_.OverrideAi(actor_base, ai);
    const u64 actor = PlacedActor(actor_base);
    if (actor == 0)
      return;
    // The six AIDT values are the six actor values of the same names, which is
    // how a script reads them back (GetActorValue("Aggression")).
    const script::papyrus::ObjectRef ref{actor};
    bindings_.SetActorValue(ref, "Aggression", static_cast<f32>(ai.aggression));
    bindings_.SetActorValue(ref, "Confidence", static_cast<f32>(ai.confidence));
    bindings_.SetActorValue(ref, "Energy", static_cast<f32>(ai.energy));
    bindings_.SetActorValue(ref, "Morality", static_cast<f32>(ai.morality));
    bindings_.SetActorValue(ref, "Mood", static_cast<f32>(ai.mood));
    bindings_.SetActorValue(ref, "Assistance", static_cast<f32>(ai.assistance));
    ++ai_profiles_;
  }

  void SetFactionReaction(bethesda::GlobalFormId faction,
                          bethesda::GlobalFormId other,
                          i32 reaction) override {
    bindings_.SetReaction(Ref(faction), Ref(other), reaction);
  }

  void SetFactionInfamy(bethesda::GlobalFormId faction, u32 violent, u32 non_violent) override {
    bindings_.SetInfamy(Ref(faction), static_cast<i32>(violent), static_cast<i32>(non_violent));
    ++infamy_factions_;
  }

  void SetDialogueSaid(bethesda::GlobalFormId info) override {
    said_.MarkSaid(info.packed());
  }

  // Cleared state lands in two places, because two systems ask different
  // questions of it: Papyrus asks the location (Location.IsCleared), and the map
  // asks the marker. The link between them is the LCTN's own MNAM, so it is
  // resolved here rather than stored twice by the decoder.
  void SetLocationCleared(bethesda::GlobalFormId location, bool cleared) override {
    bethesda::Record record;
    if (!records_.Parse(location, &record) || record.header.type != kLctn) {
      ++unknown_refs_;
      return;
    }
    script::skyrim::state::SetFlag(Ref(location), "cleared", cleared);
    ++locations_cleared_;
    const bethesda::Subrecord* marker = record.Find(kMnam);
    if (!marker || marker->data.size() < 4)
      return;
    u32 raw;
    std::memcpy(&raw, marker->data.data(), 4);
    const bethesda::RecordStore::StoredRecord* stored = records_.Find(location);
    const bethesda::GlobalFormId ref =
        records_.ResolveFrom(bethesda::RawFormId{raw}, stored ? stored->winning_plugin : 0);
    if (markers_.SetCleared(ref))
      ++markers_cleared_;
  }

  // The same table the Civil War scripts read a hold's owner out of
  // (Location.GetKeywordData), so this is not a store of its own.
  void SetLocationKeywordValue(bethesda::GlobalFormId location,
                               bethesda::GlobalFormId keyword,
                               f32 value) override {
    bindings_.SetKeywordData(Ref(location), Ref(keyword), value);
    ++location_keywords_;
  }

  void SetEncounterZoneLevel(bethesda::GlobalFormId zone, u32 level) override {
    if (!records_.Find(zone)) {
      ++unknown_refs_;
      return;
    }
    actor_stats_.SetZoneLevel(zone, level);
    ++zones_;
  }

  void AddLeveledListEntry(bethesda::GlobalFormId list,
                           bethesda::GlobalFormId form,
                           u32 level,
                           u32 count) override {
    if (!records_.Find(list) || !records_.Find(form)) {
      ++unknown_refs_;
      return;
    }
    bindings_.AddLeveledListEntry(Ref(list), Ref(form), static_cast<i32>(level),
                                  static_cast<i32>(count));
    ++leveled_entries_;
  }

  // The whole flags byte, not just "read": a skill book comes back from the save
  // with its teaches-skill bit already gone, which is what stops the resumed
  // game handing the skill out a second time.
  void SetBookRead(bethesda::GlobalFormId book, u8 flags, bool skill_taken) override {
    if (!records_.Find(book)) {
      ++unknown_refs_;
      return;
    }
    bindings_.SetBookFlags(Ref(book), flags);
    ++books_read_;
    if (skill_taken)
      ++books_skill_taken_;
  }

  void SetKnownIngredientEffects(bethesda::GlobalFormId ingredient, u32 effects) override {
    if (!records_.Find(ingredient)) {
      ++unknown_refs_;
      return;
    }
    bindings_.SetKnownIngredientEffects(Ref(ingredient), static_cast<i32>(effects));
    ++ingredients_;
  }

  // The save names the two NPC_ base forms; the engine keys relationships by the
  // placed reference, the same way faction ranks and actor values resolve.
  void SetRelationshipRank(bethesda::GlobalFormId a, bethesda::GlobalFormId b, i32 rank) override {
    const u64 first = PlacedActor(a);
    const u64 second = PlacedActor(b);
    if (first == 0 || second == 0)
      return;
    bindings_.SetRelationshipRank(script::papyrus::ObjectRef{first},
                                  script::papyrus::ObjectRef{second}, rank);
    ++relationships_;
  }

  // A cell's map bits are only worth anything once they sit on a grid, so an
  // exterior goes in by worldspace and coordinate and an interior, which has no
  // grid, records only that it was entered.
  void SetCellVisited(bethesda::GlobalFormId cell,
                      base::Span<const bethesda::CellVisitedGrid> grids) override {
    bethesda::GlobalFormId worldspace;
    i16 x = 0, y = 0;
    if (!records_.CellGridLocation(cell, &worldspace, &x, &y)) {
      if (records_.Find(cell))
        map_.MarkInterior(cell);
      else
        ++unknown_refs_;
      return;
    }
    for (const bethesda::CellVisitedGrid& grid : grids)
      map_.MarkCell(worldspace, x, y, grid.bits);
    ++visited_cells_;
  }

  // The save only carries the two flags; what the place is called and where it
  // sits came out of the records when the catalogue was built, so a marker the
  // load order does not place is counted rather than invented.
  void SetMapMarker(bethesda::GlobalFormId ref, bool visible, bool can_travel) override {
    if (!markers_.SetFlags(ref, visible, can_travel)) {
      ++unknown_markers_;
      return;
    }
    ++markers_found_;
    if (can_travel)
      ++markers_travel_;
  }

  void SetReferenceEnabled(bethesda::GlobalFormId ref, bool enabled) override {
    // Only the references whose state differs from the one the records place
    // them in are worth a command: the rest already come up right.
    const bethesda::RecordStore::StoredRecord* stored = records_.Find(ref);
    if (!stored) {
      ++unknown_refs_;
      return;
    }
    if (((stored->header.flags & kRecordFlagInitiallyDisabled) == 0) == enabled)
      return;
    bindings_.SetEnabled(Ref(ref), enabled);
    ++toggled_refs_;
  }

  // Binned, not spawned: a save carries tens of thousands of these and only the
  // cells in the load ring are ever standing, so the streamer places each cell's
  // share when that cell comes in (see saved_spawns.h).
  void SpawnCreatedReference(bethesda::GlobalFormId id,
                             bethesda::GlobalFormId base,
                             bethesda::GlobalFormId parent,
                             const f32 position[3],
                             const f32 rotation[3],
                             f32 scale,
                             bool actor) override {
    // Without a base record there is nothing to render or collide with, and
    // without a parent record there is no cell to bin it into.
    if (!records_.Find(base)) {
      ++unknown_refs_;
      return;
    }
    const bethesda::RecordStore::StoredRecord* stored = records_.Find(parent);
    if (!stored) {
      ++unknown_refs_;
      return;
    }
    world::SavedSpawn spawn;
    spawn.handle = id;
    spawn.base = base;
    for (u32 axis = 0; axis < 3; ++axis) {
      spawn.position[axis] = position[axis];
      spawn.rotation[axis] = rotation[axis];
    }
    spawn.scale = scale;
    spawn.actor = actor;
    if (stored->header.type == kWrld) {
      spawns_.AddExterior(parent, cell_size_, spawn);
      ++spawns_exterior_;
    } else if (stored->header.type == kCell) {
      spawns_.AddInterior(parent, spawn);
      ++spawns_interior_;
    } else {
      ++unknown_refs_;
    }
  }

  void MoveReference(bethesda::GlobalFormId ref,
                     bethesda::GlobalFormId parent,
                     const f32 position[3],
                     const f32 rotation[3]) override {
    const bethesda::RecordStore::StoredRecord* stored = records_.Find(ref);
    if (!stored) {
      ++unknown_refs_;
      return;
    }
    // Game units in, exactly like the Papyrus SetPosition this shares a store
    // with; the world sink converts.
    bindings_.SetPosition(Ref(ref), position[0], position[1], position[2]);
    ++moved_refs_;
    // A reference that only shifted inside its own cell is placed by that cell
    // and moved by the command above once it stands. One that left its cell has
    // to be placed by the cell it went to instead, or it comes back wherever its
    // record puts it and disappears with a cell it is no longer in.
    if (!LeftItsCell(ref, parent, position))
      return;
    bethesda::Record record;
    if (!records_.Parse(ref, &record))
      return;
    const bethesda::Subrecord* name = record.Find(kName);
    if (!name || name->data.size() < 4) {
      ++unknown_refs_;
      return;
    }
    u32 raw;
    std::memcpy(&raw, name->data.data(), 4);
    world::SavedSpawn spawn;
    spawn.handle = ref;
    spawn.base = records_.ResolveFrom(bethesda::RawFormId{raw}, stored->winning_plugin);
    if (!records_.Find(spawn.base)) {
      ++unknown_refs_;
      return;
    }
    for (u32 axis = 0; axis < 3; ++axis) {
      spawn.position[axis] = position[axis];
      spawn.rotation[axis] = rotation[axis];
    }
    // The save carries no scale for a reference it only moved, so the record's
    // own is still the right one.
    if (const bethesda::Subrecord* scale = record.Find(kXscl); scale && scale->data.size() >= 4)
      std::memcpy(&spawn.scale, scale->data.data(), 4);
    spawn.actor = stored->header.type == kAchr;
    spawn.relocated = true;
    if (const bethesda::RecordStore::StoredRecord* home = records_.Find(parent);
        home && home->header.type == kWrld) {
      spawns_.AddExterior(parent, cell_size_, spawn);
    } else {
      spawns_.AddInterior(parent, spawn);
    }
    ++relocated_refs_;
  }

  // A potion the player brewed or an enchantment they made. The save's table is
  // the only description there is, so the name is built the way the game builds
  // it: after the effect the thing mostly does. Everything else comes off that
  // effect's own record, which the load order does have.
  void AddCreatedForm(bethesda::GlobalFormId id,
                      bethesda::CreatedFormKind kind,
                      base::Span<const bethesda::CreatedFormEffect> effects) override {
    if (effects.empty())
      return;
    const bool consumable = kind == bethesda::CreatedFormKind::kPotion ||
                            kind == bethesda::CreatedFormKind::kPoison;
    world::CreatedForm form;
    form.consumable = consumable;
    // The price the game computed sits on the first effect and is 0 on the rest.
    form.value = static_cast<u32>(base::Max(0.0f, effects[0].value));
    // Alchemy products all weigh the same in Skyrim; an enchantment is not an
    // item of its own and weighs nothing.
    form.weight = consumable ? 0.5f : 0.0f;

    // The strongest effect names the thing, which is how the game names it, and
    // magnitude is the only ordering the table gives.
    const bethesda::CreatedFormEffect* strongest = &effects[0];
    for (const bethesda::CreatedFormEffect& effect : effects)
      if (effect.magnitude > strongest->magnitude)
        strongest = &effect;
    base::String effect_name;
    bethesda::Record record;
    if (records_.Parse(strongest->effect, &record) && record.header.type == kMgef)
      effect_name = DisplayName(record);
    if (effect_name.empty())
      effect_name = "Unknown";
    switch (kind) {
      case bethesda::CreatedFormKind::kPotion:
        form.name = "Potion of " + effect_name;
        break;
      case bethesda::CreatedFormKind::kPoison:
        form.name = "Poison of " + effect_name;
        break;
      default:
        form.name = effect_name;  // an enchantment is named after its effect
        break;
    }
    created_forms_.Add(id, form);
    bindings_.SetName(Ref(id), form.name);
    ++created_forms_added_;
  }

  // The save stores container contents as a signed delta against what the
  // records author, so the authored contents have to be in the store before the
  // delta means anything. Seeding them here rather than at streaming time is
  // what makes a looted chest come back empty instead of full.
  void AddContainerItem(bethesda::GlobalFormId container,
                        bethesda::GlobalFormId item,
                        i32 delta,
                        bool equipped) override {
    if (!KnownReference(container)) {
      ++unknown_refs_;
      return;
    }
    // A form the save invented that described nothing this load order has (its
    // effects named no records) is not a thing anyone could hold.
    if (!KnownItem(item)) {
      ++unknown_refs_;
      return;
    }
    if (seeded_.insert(container.packed())) {
      bindings_.SeedAuthoredInventory(Ref(container));
      ++seeded_containers_;
    }
    const i32 total = bindings_.GetItemCount(Ref(container), Ref(item)) + delta;
    // A delta below what the records author means the save's container held
    // more than the record does (a levelled list rolled it), and there is no
    // way to know how much more, so the stack empties rather than go negative.
    bindings_.SetItemCount(Ref(container), Ref(item), base::Max(0, total));
    if (equipped && total > 0)
      bindings_.EquipItem(Ref(container), Ref(item));
    ++inventory_items_;
  }

  void LogTally() const {
    RX_INFO(
        "save: {} actor values, {} faction ranks, {} references toggled, {} moved ({} of them out "
        "of the cell their record is in, and re-homed to the one they stand in)",
        actor_values_, faction_ranks_, toggled_refs_, moved_refs_, relocated_refs_);
    RX_INFO(
        "save: {} actor levels and {} ai profiles onto placed actors, {} crime factions carry "
        "infamy, {} dialogue lines already said, {} exterior cells and {} interiors on the map",
        actor_levels_, ai_profiles_, infamy_factions_, said_.size(), visited_cells_,
        map_.VisitedInteriors());
    RX_INFO("save: {} map markers discovered, {} of them fast travel destinations",
            markers_found_, markers_travel_);
    RX_INFO(
        "save: {} locations cleared ({} of them showing on the map), {} location keyword values, "
        "{} encounter zones hold the level they locked to",
        locations_cleared_, markers_cleared_, location_keywords_, zones_);
    RX_INFO("save: {} books read ({} skill books already paid out), {} ingredients carry known "
            "effects, {} entries added to leveled lists, {} actor relationships",
            books_read_, books_skill_taken_, ingredients_, leveled_entries_, relationships_);
    if (unknown_markers_ != 0)
      RX_WARN("save: {} discovered markers name a reference this load order does not place",
              unknown_markers_);
    RX_INFO("save: {} item stacks restored into {} containers seeded from their records",
            inventory_items_, seeded_containers_);
    if (created_forms_added_ != 0)
      RX_INFO("save: {} potions, poisons and enchantments the player made are back, named after "
              "what they do",
              created_forms_added_);
    RX_INFO("save: {} actor values onto references, {} of them the player's own", reference_values_,
            player_values_);
    if (perks_ != 0 || unknown_perks_ != 0)
      RX_INFO("save: {} perks onto references, {} named no PERK record here", perks_,
              unknown_perks_);
    RX_INFO(
        "save: {} spells and {} shouts restored, {} words of power known, {} magic favourites; "
        "the shout in hand is {}",
        spells_, shouts_, words_, favourites_,
        last_shout_.plugin == 0xffff ? base::String("none")
                                     : Fmt("0x%02x%06x", last_shout_.plugin, last_shout_.local_id));
    if (unknown_spells_ != 0 || unknown_words_ != 0)
      RX_WARN("save: {} spells and {} words name no record of their kind here", unknown_spells_,
              unknown_words_);
    if (names_ != 0 || races_ != 0 || faces_applied_ != 0)
      RX_INFO("save: {} actors renamed, {} put in another race, {} carry their own face", names_,
              races_, faces_applied_);
    RX_INFO(
        "save: {} references the save created are queued for their cells ({} outside, {} in "
        "interiors), across {} cells, {} in the fullest",
        spawns_exterior_ + spawns_interior_, spawns_exterior_, spawns_interior_,
        u32(spawns_.cells()), u32(spawns_.busiest_cell()));
    if (unplaced_actors_ != 0)
      RX_WARN("save: {} actor bases have no placed reference, their state was dropped",
              unplaced_actors_);
    if (unknown_refs_ != 0)
      RX_WARN("save: {} references name a record this load order does not have", unknown_refs_);
  }

 private:
  static script::papyrus::ObjectRef Ref(bethesda::GlobalFormId id) {
    return script::papyrus::ObjectRef{id.packed()};
  }

  // Whether the id names a record of that signature in this load order. The
  // remap answers for any form the plugin holds, so an id that lands on some
  // other record type is one the save meant for a plugin this run does not have.
  bool IsRecord(bethesda::GlobalFormId id, u32 signature) const {
    const bethesda::RecordStore::StoredRecord* stored = records_.Find(id);
    return stored != nullptr && stored->header.type == signature;
  }

  // A reference the running game can be told about. Normally that means a
  // plugin authors a record for it, with one exception: the player's own
  // reference is one of the forms the engine defines itself and no plugin
  // writes, so requiring a record here silently drops everything the save says
  // about the player, their whole pack included.
  bool KnownReference(bethesda::GlobalFormId ref) const {
    if (ref.plugin == 0 && ref.local_id == bethesda::kPlayerFormId)
      return true;
    return records_.Find(ref) != nullptr;
  }

  // A form the inventory can hold: a record, or one of the forms the save made.
  bool KnownItem(bethesda::GlobalFormId item) const {
    return item.plugin == bethesda::kCreatedFormPlugin ? created_forms_.Find(item) != nullptr
                                                       : records_.Find(item) != nullptr;
  }

  // A record's displayed name. A localized plugin stores a 4-byte string id in
  // FULL and the text lives in the .STRINGS tables; a non-localized one stores
  // the text itself.
  base::String DisplayName(const bethesda::Record& record) const {
    const bethesda::Subrecord* full = record.Find(kFull);
    if (!full)
      return {};
    if (full->data.size() >= 4) {
      u32 string_id;
      std::memcpy(&string_id, full->data.data(), 4);
      if (const base::String* text = strings_.Find(string_id))
        return base::String(text->c_str());
    }
    return record.GetString(kFull);
  }

  // Whether the save left a reference somewhere other than the cell its record
  // is authored in. `parent` is the cell it now sits in, or the worldspace when
  // it sits outside, and `position` is where in that space.
  //
  // The two exterior cases are decided on the grid coordinate rather than the
  // cell form: the save names only the worldspace for a reference standing
  // outside, so there is no cell id on either side to compare. That also means a
  // reference carried between two worldspaces is only caught when its
  // coordinates change with it, which in practice they always do.
  bool LeftItsCell(bethesda::GlobalFormId ref,
                   bethesda::GlobalFormId parent,
                   const f32 position[3]) const {
    const bethesda::GlobalFormId interior = records_.InteriorCellOfRef(ref);
    const bethesda::RecordStore::StoredRecord* home = records_.Find(parent);
    if (!home)
      return false;
    if (home->header.type != kWrld)
      return interior.plugin == 0xffff || interior.packed() != parent.packed();
    // Outside now. A reference the records put in an interior has left it, and
    // one that was already outside has only left if its grid cell changed.
    if (interior.plugin != 0xffff)
      return true;
    bethesda::Record record;
    const bethesda::Subrecord* data = nullptr;
    if (!records_.Parse(ref, &record) || !(data = record.Find(kData)) || data->data.size() < 24)
      return false;
    f32 authored[3];
    std::memcpy(authored, data->data.data(), sizeof(authored));
    return Grid(authored[0]) != Grid(position[0]) || Grid(authored[1]) != Grid(position[1]);
  }

  i32 Grid(f32 game_units) const {
    return cell_size_ > 0.0f ? static_cast<i32>(std::floor(game_units / cell_size_)) : 0;
  }

  // Actor state is saved against the NPC base form; the engine keys it by the
  // placed reference. Unique NPCs have exactly one, which is what a save's
  // per-actor state describes.
  u64 PlacedActor(bethesda::GlobalFormId base) {
    // The player's base form has no placement to look up: its one reference is
    // the fixed player id, in Skyrim and in Fallout 4 alike.
    if (base.plugin == 0 && base.local_id == kPlayerBaseFormId)
      return bethesda::GlobalFormId{0, bethesda::kPlayerFormId}.packed();
    // Cached so an actor with no placement is reported once rather than once
    // per value the save carries for it.
    if (const u64* cached = placed_actors_.find(base.packed()))
      return *cached;
    const bethesda::GlobalFormId ref = records_.PlacedRefForBase(base);
    const u64 handle = ref.plugin == 0xffff ? 0 : ref.packed();
    placed_actors_[base.packed()] = handle;
    if (handle == 0)
      ++unplaced_actors_;
    return handle;
  }

  script::skyrim::RecordBackedSkyrimBindings& bindings_;
  const bethesda::RecordStore& records_;
  world::ActorStatsStore& actor_stats_;
  dialogue::SaidTopics& said_;
  world::MapDiscovery& map_;
  world::MapMarkers& markers_;
  world::SavedSpawnIndex& spawns_;
  world::MiscStats& misc_stats_;
  world::CreatedForms& created_forms_;
  const bethesda::StringTable& strings_;
  u32 created_forms_added_ = 0;
  bethesda::GlobalFormId weather_;
  FaceBuilder& faces_;
  f32 cell_size_ = 0.0f;
  u32 spawns_exterior_ = 0;
  u32 spawns_interior_ = 0;
  base::UnorderedMap<u64, u64> placed_actors_;  // NPC base -> placed ref, 0 = none
  u32 actor_values_ = 0;
  u32 reference_values_ = 0;
  u32 player_values_ = 0;
  u32 perks_ = 0;
  u32 unknown_perks_ = 0;
  u32 spells_ = 0;
  u32 unknown_spells_ = 0;
  u32 shouts_ = 0;
  u32 words_ = 0;
  u32 unknown_words_ = 0;
  u32 favourites_ = 0;
  u32 names_ = 0;
  u32 races_ = 0;
  u32 faces_applied_ = 0;
  bethesda::GlobalFormId last_shout_;
  u32 faction_ranks_ = 0;
  u32 toggled_refs_ = 0;
  u32 moved_refs_ = 0;
  u32 relocated_refs_ = 0;
  u32 actor_levels_ = 0;
  u32 ai_profiles_ = 0;
  u32 infamy_factions_ = 0;
  u32 visited_cells_ = 0;
  u32 markers_found_ = 0;
  u32 markers_travel_ = 0;
  u32 unknown_markers_ = 0;
  u32 unplaced_actors_ = 0;
  u32 unknown_refs_ = 0;
  u32 locations_cleared_ = 0;
  u32 markers_cleared_ = 0;
  u32 location_keywords_ = 0;
  u32 zones_ = 0;
  u32 leveled_entries_ = 0;
  u32 books_read_ = 0;
  u32 books_skill_taken_ = 0;
  u32 ingredients_ = 0;
  u32 relationships_ = 0;
  base::UnorderedSet<u64> seeded_;  // containers already given their authored contents
  u32 seeded_containers_ = 0;
  u32 inventory_items_ = 0;
};

}  // namespace

bool LoadSavegame(Engine& engine, const bethesda::LoadOrder& order) {
  Engine* const self = &engine;
  if (self->config_.load_save.empty() && LoadSaveFile.get())
    self->config_.load_save = LoadSaveFile.get();
  if (self->config_.load_save.empty())
    return true;

  const base::String& path = self->config_.load_save;
  base::Vector<u8> bytes;
  if (!ReadWholeFile(path, &bytes)) {
    RX_ERROR("save: cannot read {}", path);
    return false;
  }

  auto save = base::MakeUnique<LoadedSavegame>();
  if (!bethesda::ReadSaveFile(ByteSpan(bytes.data(), bytes.size()), save->file)) {
    RX_ERROR("save: {} is not a savegame this reader understands", path);
    return false;
  }
  if (!GameMatchesSave(self->game_, save->file.format)) {
    RX_ERROR("save: {} was written by a different game than the one loading", path);
    return false;
  }

  // The remap has to be built here, while the order this run actually loaded is
  // still in hand: every id in the file is relative to the save's own list.
  save->remap.Build(save->file, [&order](const base::String& name) { return order.IndexOf(name); });
  for (const base::String& missing : save->remap.missing_plugins())
    RX_WARN("save: {} is not loaded, every record from it is refused", missing);

  RX_INFO("save: {} level {} in {}, {} plugins, {} change forms, {} globals",
          save->file.player_name, save->file.player_level, save->file.player_location,
          save->file.plugins.size(), save->file.change_forms.size(), save->file.globals.size());

  if (!bethesda::FindPlayerPlacement(save->file, save->remap, &save->player))
    RX_WARN("save: no usable player placement, booting at the default start cell");

  self->save_ = base::move(save);
  return true;
}

void ApplySavegameState(Engine& engine) {
  Engine* const self = &engine;
  if (!self->save_ || !self->script_bindings_ || !self->scripts_)
    return;

  // Every level-mult actor scales against the player, so this has to be set
  // before the first actor resolves against it.
  self->actor_stats_.set_player_level(self->save_->file.player_level);

  bethesda::SaveApplyStats stats;
  EngineSaveSink sink(*self->script_bindings_, self->records_, self->actor_stats_,
                      self->said_topics_, self->map_discovery_, self->map_markers_,
                      self->saved_spawns_, self->misc_stats_, self->created_forms_,
                      self->strings_, self->actors_->faces(),
                      bethesda::GameProfile::For(self->game_).cell_size);
  // On the guest thread and synchronously: everything below is guest-owned
  // state, and the quest scripts that read it attach right after this returns.
  PlayerReadout readout;
  const auto started = std::chrono::steady_clock::now();
  self->scripts_->guest().Dispatch([&](script::papyrus::VirtualMachine&) {
    bethesda::ApplySave(self->save_->file, self->save_->remap, sink, &stats);
    readout = ReadPlayerBack(*self->script_bindings_);
  });
  const auto apply_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();

  RX_INFO(
      "save: applied {} globals, {} quests ({} stages, {} objectives, {} alias fills), {} actors",
      stats.globals, stats.quests, stats.quest_stages, stats.quest_objectives, stats.quest_aliases,
      stats.actors);
  RX_INFO(
      "save: applied {} actor levels, {} ai profiles, {} faction infamy counts, "
      "{} spoken dialogue lines, {} visited map grids, {} map markers ({} travelable)",
      stats.actor_levels, stats.actor_ai_profiles, stats.faction_infamy, stats.dialogue_said,
      stats.cells_visited, stats.map_markers, stats.map_markers_travel);
  RX_INFO(
      "save: decoded {} locations ({} cleared, {} keyword values), {} encounter zones, "
      "{} leveled lists ({} entries), {} books read ({} skill books), {} ingredients "
      "({} effects), {} relationships",
      stats.locations, stats.locations_cleared, stats.location_keywords, stats.encounter_zones,
      stats.leveled_lists, stats.leveled_entries, stats.books_read, stats.books_skill_taken,
      stats.ingredients, stats.ingredient_effects, stats.relationships);
  sink.LogTally();
  RX_INFO("save: {} form ids remapped, {} refused (created at runtime), {} change forms dropped",
          stats.forms.mapped, stats.forms.created, stats.refused);
  if (self->clock_)
    RX_INFO("save: resumed at game hour {:.2f} of day {}", self->clock_->hour(),
            self->clock_->game_days());
  RX_INFO("save: applied {} inventories ({} item stacks), {} perks on {} references",
          stats.inventories, stats.inventory_items, stats.actor_perks, stats.actors_with_perks);
  // Systems the save carries state for that the engine has nowhere to put yet.
  RX_INFO(
      "save: {} of the {} forms the save invented describe something this load order has, and {} "
      "item stacks name one",
      stats.created_forms_with_effects, stats.created_forms, stats.inventory_items_created);
  RX_INFO("save: not applied: {} inventories read only in part, {} cells whose owner changed",
          stats.inventories_incomplete, stats.cells_owned);
  RX_INFO(
      "save: {} references the save created, {} name a base form this load order has, {} handed "
      "to the streamer, {} dropped as disabled or deleted",
      stats.created_references, stats.created_references_with_base,
      stats.created_references_spawned, stats.created_references_inert);
  RX_INFO("save: applied in {} ms", apply_ms);

  // The Papyrus heap. Not applied here: only the quest scripts exist at this
  // point, and a reference's scripts attach when its cell streams in. So the
  // index is built now, while the remap is in hand, and every attachment from
  // here on (starting with the quest scripts, right after this returns) asks it
  // what the save held for that instance.
  if (self->save_->file.papyrus.present) {
    const auto papyrus_started = std::chrono::steady_clock::now();
    self->papyrus_restore_ = base::MakeUnique<script::PapyrusRestorer>();
    self->papyrus_restore_->Build(base::move(self->save_->file.papyrus), self->save_->remap);
    script::PapyrusRestorer* restorer = &*self->papyrus_restore_;
    self->scripts_->set_on_script_restored(
        [restorer](script::papyrus::VirtualMachine& vm, script::papyrus::ObjectRef instance,
                   const base::String& script) {
          return restorer->Apply(vm, instance, script);
        });
    const auto papyrus_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - papyrus_started)
                                .count();
    RX_INFO("papyrus: heap indexed in {} ms", papyrus_ms);
    self->papyrus_restore_->LogCoverage();
  }

  // The Stats page and the sky. Both come out of the save's global data rather
  // than its change forms, so neither depends on a record being placed.
  if (stats.misc_stats != 0)
    RX_INFO("save: {} stats rows, {} days passed, {} locations found, {} people killed, "
            "{} lifetime bounty",
            stats.misc_stats, self->misc_stats_.Value("Days Passed"),
            self->misc_stats_.Value("Locations Discovered"),
            self->misc_stats_.Value("People Killed"),
            self->misc_stats_.Value("Total Lifetime Bounty"));
  if (stats.ingredient_pairs != 0)
    RX_INFO("save: {} ingredient combinations remembered", stats.ingredient_pairs);

  // The sky the save was left under. Without a placement the region lookup
  // would run at the origin, which is a different climate from wherever the
  // player actually is.
  if (stats.weather && self->clock_ && self->save_->player.valid) {
    const bethesda::GlobalFormId id = sink.weather();
    const f32* at = self->save_->player.position;
    weather::WeatherDef def;
    if (!weather::LoadWeather(self->records_, id, &def)) {
      RX_WARN("save: weather {:04x}:{:06x} names no WTHR record here, the sky rolls fresh",
              id.plugin, id.local_id);
    } else if (self->director_.ResumeWeather(def, self->clock_->game_days(), at[0], at[1])) {
      RX_INFO("save: resumed the sky on '{}'", def.editor_id);
    } else {
      RX_WARN("save: could not seat weather '{}' in the climate, the sky rolls fresh",
              def.editor_id);
    }
  }
  // Read straight back out of the live systems rather than out of the decoder,
  // so the line says what the running game believes the player is.
  RX_INFO(
      "save: the player is now level {}, {:.0f}/{:.0f}/{:.0f} health/magicka/stamina, "
      "carrying {:.0f}, one-handed {:.0f} sneak {:.0f} destruction {:.0f}, "
      "{} perks and {} gold",
      readout.level, readout.health, readout.magicka, readout.stamina, readout.carry_weight,
      readout.one_handed, readout.sneak, readout.destruction, readout.perks, readout.gold);
  RX_INFO("save: {} answers to {} spells, {} shouts, {} words of power and {} favourites",
          readout.name.empty() ? base::String("the player") : readout.name, readout.spells,
          readout.shouts, readout.words, readout.favourites);
}

void ApplySavegameLocation(Engine& engine) {
  Engine* const self = &engine;
  if (!self->save_ || !self->save_->player.valid)
    return;
  const bethesda::GlobalFormId parent = self->save_->player.parent;
  const bethesda::RecordStore::StoredRecord* stored = self->records_.Find(parent);
  if (!stored) {
    RX_WARN("save: the player's cell is not in this load order, using the default start cell");
    return;
  }

  if (stored->header.type == kCell) {
    // Hand the interior to the existing boot path, which takes a load order
    // form id in the same "0x" spelling --interior does. That spelling only has
    // room for a one byte plugin index, which every real load order fits in.
    if (parent.plugin > 0xff) {
      RX_WARN("save: the player's interior cell is in plugin {}, past the form id spelling",
              parent.plugin);
      return;
    }
    self->config_.interior = Fmt("0x%02x%06x", parent.plugin, parent.local_id);
    RX_INFO("save: player is in interior cell {}", self->config_.interior);
    return;
  }
  if (stored->header.type != kWrld) {
    RX_WARN("save: the player's parent form is neither a cell nor a worldspace");
    return;
  }

  bethesda::Record record;
  if (self->records_.Parse(parent, &record))
    self->save_->worldspace = record.GetString(kEdid);
  const auto& profile = bethesda::GameProfile::For(self->game_);
  const f32* position = self->save_->player.position;
  self->config_.start_cell_x = static_cast<i32>(std::floor(position[0] / profile.cell_size));
  self->config_.start_cell_y = static_cast<i32>(std::floor(position[1] / profile.cell_size));
  self->config_.start_cell_explicit = true;
  RX_INFO("save: player is in worldspace {} cell {},{}",
          self->save_->worldspace.empty() ? base::String("(unnamed)") : self->save_->worldspace,
          self->config_.start_cell_x, self->config_.start_cell_y);
}

void PlaceSavegamePlayer(Engine& engine) {
  Engine* const self = &engine;
  if (!self->save_)
    return;
  if (!self->save_->player.valid) {
    self->save_ = {};
    return;
  }
  const auto& profile = bethesda::GameProfile::For(self->game_);
  const f32 scale = profile.units_to_meters;
  const f32* position = self->save_->player.position;
  // Bethesda space (Z up) to engine space (Y up, metres), the same axis change
  // every streamed reference goes through.
  Vec3 feet{position[0] * scale, position[2] * scale, -position[1] * scale};
  // Game euler z is the compass yaw, which is already the engine's camera yaw.
  const f32 yaw = self->save_->player.rotation[2];

  // The save's height is the game's own terrain, and this engine bakes its from
  // the same LAND records, but not to the same surface: the heightfield samples
  // a 33x33 grid per cell and the game's collision mesh does not, so a save left
  // on a slope lands a few centimetres inside ours. A capsule that starts inside
  // the ground is resolved DOWNWARD by the character solver and shoots hundreds
  // of metres under the world, which is why a resumed save used to open on an
  // empty grey plane. So the terrain has the last word on the height, and the
  // player is set down a little clear of it rather than exactly on it. Only ever
  // upwards: a save on a rooftop or a bridge is above the terrain and stays.
  if (self->streamer_ && !self->streamer_->in_interior()) {
    f32 ground = 0;
    if (!self->streamer_->GroundHeight(feet.x, feet.z, &ground)) {
      RX_WARN("save: no terrain under the player's resume position, standing where the save says");
    } else if (feet.y < ground + world::CellStreamer::kGroundClearance) {
      RX_INFO("save: the resume position is {:.2f} m into the terrain, setting down on top of it",
              ground - feet.y);
      feet.y = ground + world::CellStreamer::kGroundClearance;
    }
  }

  self->actors_->TeleportPlayer(feet.x, feet.y, feet.z);
  // The body too, not just the camera: walk mode seeds its heading off the
  // biped when the character controller assembles, which happens after this.
  // The biped mesh faces +Z about engine up, so the compass yaw turns around.
  self->actors_->SetPlayerFacing(3.14159265f - yaw);
  self->camera_.set_position({feet.x, feet.y + 1.8f, feet.z});
  self->camera_.set_yaw_pitch(yaw, 0.0f);
  self->ctx_.cam_yaw = yaw;
  RX_INFO("save: player resumes at ({:.1f}, {:.1f}, {:.1f}) engine / ({:.1f}, {:.1f}, {:.1f}) game",
          feet.x, feet.y, feet.z, position[0], position[1], position[2]);
  // Placing the player is the last thing the save is read for, so let its
  // hundred thousand change forms go rather than hold them for the session.
  self->save_ = {};
}

}  // namespace rx
