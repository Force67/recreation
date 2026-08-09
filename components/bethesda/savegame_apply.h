#ifndef COMPONENTS_BETHESDA_SAVEGAME_APPLY_H
#define COMPONENTS_BETHESDA_SAVEGAME_APPLY_H

// Layer 3 of the savegame reader (see savegame.h): the decoded save onto the
// live world.
//
// Two things live here. FormRemap turns a form id that only means something in
// the save's load order into one that means the same thing in the running
// game's, and refuses ids whose plugin is not loaded rather than landing on
// whichever record now occupies that index. ApplySave then walks the change
// forms and pushes what it understands at a SaveSink.
//
// Nothing here knows about the ECS, quests or Papyrus. Keeping the write side
// behind SaveSink is what lets applying be tested without a world, and what
// keeps Skyrim and Fallout 4 on one path: the sink is the only place a game's
// runtime shape shows up.

#include <base/containers/span.h>
#include <base/containers/vector.h>
#include <base/functional/function.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include "components/bethesda/form_id.h"
#include "components/bethesda/savegame.h"
#include "components/bethesda/savegame_changeform.h"
#include "core/types.h"

namespace rx::bethesda {

// The player's own reference, the same id in every game this reader covers.
constexpr u32 kPlayerFormId = 0x00000014;

// The load order slot a reference the save created at runtime is handed. No
// plugin can occupy it (a load order holds at most 255 full plugins plus the
// ESL range) and nothing else in the engine claims it, so a created reference
// gets a stable handle that cannot collide with a record's, and the slot alone
// says where the handle came from.
constexpr u16 kCreatedReferencePlugin = 0xfffd;

// Why a form id could not be carried into the running game. Counted rather
// than logged one by one: a save names six figures of them.
struct RemapCounters {
  u32 mapped = 0;
  u32 missing_plugin = 0;  // the save's plugin is not in the running load order
  u32 out_of_range = 0;    // mod index past the save's own plugin list
  u32 created = 0;         // 0xFFxxxxxx, a form the save itself invented
};

// Save load order -> runtime load order.
//
// A save stores every id already resolved against its own plugin list, so the
// top byte (or the ESL slot) means "the plugin that sat at this index when the
// save was written". Nothing about the running game guarantees the same index
// holds the same plugin, and a wrong answer here is silent: it lands on a real
// record of the wrong mod. So the mapping goes through the plugin's file name,
// and a name the runtime does not have refuses every id that came from it.
class FormRemap {
 public:
  // `runtime_index` answers with the running game's load order index for a
  // plugin file name, or 0xffff when it is not loaded (LoadOrder::IndexOf).
  void Build(const SaveFile& save, const base::Function<u16(const base::String&)>& runtime_index);

  // False when the id cannot be carried over; `reason` (optional) says which
  // RemapCounters field the caller should charge it to.
  enum class Refusal : u8 { kNone, kMissingPlugin, kOutOfRange, kCreated };
  bool Map(u32 save_form_id, GlobalFormId* out, Refusal* reason = nullptr) const;

  // Remaps a ref embedded in a change form payload, resolving it through the
  // save's form id map first.
  bool MapRef(ChangeRef ref, const SaveFile& save, GlobalFormId* out) const;

  // Plugins the save was written with that the running game has not loaded.
  // Every id from them is refused, so this is the list to show a player.
  const base::Vector<base::String>& missing_plugins() const { return missing_plugins_; }
  bool built() const { return built_; }

 private:
  // Save mod index -> runtime load order index, 0xffff for "not loaded". Only
  // the first plugin_count_ entries are written, and only those are read.
  u16 mod_[256];
  // Save ESL slot -> runtime index. Light plugins do not occupy a mod index:
  // their forms live at 0xFExxxxxx with the slot in the next 12 bits.
  base::Vector<u16> light_;
  base::Vector<base::String> missing_plugins_;
  u8 plugin_count_ = 0;
  bool built_ = false;
};

// The running game, as layer 3 needs to see it. Every id handed over is
// already remapped. Methods default to doing nothing, because "the engine has
// no home for this yet" is the normal case for a system it has not built.
class SaveSink {
 public:
  virtual ~SaveSink() = default;

  virtual void SetGlobal(GlobalFormId global, f32 value) {}

  // Stages arrive in ascending order, each one the save recorded as run, then
  // SetQuestState closes the quest out with the state the journal shows.
  virtual void SetQuestStageDone(GlobalFormId quest, i32 stage) {}
  virtual void SetQuestState(GlobalFormId quest, i32 stage, bool running, bool complete) {}
  virtual void SetQuestObjective(GlobalFormId quest,
                                 i32 objective,
                                 bool displayed,
                                 bool completed) {}
  virtual void FillQuestAlias(GlobalFormId quest, u32 alias_id, GlobalFormId ref) {}

  // Actor state is keyed by the NPC base form, because that is what the save
  // changes; resolving a base to its placed reference is the runtime's job.
  virtual void SetActorValue(GlobalFormId actor_base, base::StringRef name, f32 value) {}
  // The values an ACHR carries are the reference's own, not its base form's:
  // they are what the actor levelled its way to, and no other actor sharing
  // that base has them. The player's skills and vitals arrive here.
  virtual void SetReferenceActorValue(GlobalFormId ref, base::StringRef name, f32 value) {}
  // A perk the save records on that reference, keyed like the reference values
  // above: perks are earned, so they belong to the actor and not to its base.
  virtual void AddReferencePerk(GlobalFormId ref, GlobalFormId perk, u32 rank) {}
  virtual void SetActorFactionRank(GlobalFormId actor_base, GlobalFormId faction, i32 rank) {}
  // Already a level, never the multiplier: the save's own player level resolves
  // a level-mult actor before it gets here.
  virtual void SetActorLevel(GlobalFormId actor_base, u32 level) {}
  virtual void SetActorAi(GlobalFormId actor_base, const ActorAi& ai) {}
  virtual void SetFactionReaction(GlobalFormId faction, GlobalFormId other, i32 reaction) {}
  // How many crimes of each kind the faction has seen the player commit. Not a
  // bounty: infamy survives paying one off (see FactionChange).
  virtual void SetFactionInfamy(GlobalFormId faction, u32 violent, u32 non_violent) {}

  // A dialogue response the player has already heard.
  virtual void SetDialogueSaid(GlobalFormId info) {}
  // Map exploration for one cell: `grids` is the uncovered-tile bitmap the save
  // stores, empty for a cell that is only known to have been entered.
  virtual void SetCellVisited(GlobalFormId cell, base::Span<const CellVisitedGrid> grids) {}
  // A named location the player has found. `ref` is the map marker's own
  // reference; what it is called and where it sits stay in that record, so a
  // sink with no catalogue of markers cannot do anything with this.
  virtual void SetMapMarker(GlobalFormId ref, bool visible, bool can_travel) {}

  virtual void SetReferenceEnabled(GlobalFormId ref, bool enabled) {}
  // A reference the save itself created (a dropped weapon, a corpse, a critter
  // a script spawned). No plugin has a record for it, so the base form and the
  // transform below are its whole description and `id` is a synthetic handle in
  // the created-reference slot. `parent` is the cell it sits in, or the
  // worldspace when it sits outside; `actor` marks an ACHR.
  virtual void SpawnCreatedReference(GlobalFormId id,
                                     GlobalFormId base,
                                     GlobalFormId parent,
                                     const f32 position[3],
                                     const f32 rotation[3],
                                     f32 scale,
                                     bool actor) {}
  // `parent` is the cell the reference now sits in, or the worldspace when it
  // sits outside; position is in game units, rotation in radians.
  virtual void MoveReference(GlobalFormId ref,
                             GlobalFormId parent,
                             const f32 position[3],
                             const f32 rotation[3]) {}

  // One stack of a container's or actor's inventory. `delta` is signed and
  // relative to the contents the container's base record authors, never an
  // absolute count, so a sink that has no model of the authored contents cannot
  // apply this at all (see InventoryItem).
  virtual void AddContainerItem(GlobalFormId container,
                                GlobalFormId item,
                                i32 delta,
                                bool equipped) {}
};

// Where the player stood when the save was written.
struct PlayerPlacement {
  bool valid = false;
  // The worldspace (exterior) or cell (interior) the transform belongs to. Which
  // one it is cannot be told from the save alone: the parent is a bare form id
  // and only the record behind it says WRLD or CELL.
  GlobalFormId parent;
  f32 position[3] = {};
  f32 rotation[3] = {};  // radians, x/y/z
};

struct SaveApplyStats {
  RemapCounters forms;

  u32 globals = 0;
  u32 quests = 0;
  u32 quest_stages = 0;
  u32 quest_objectives = 0;
  u32 quest_aliases = 0;
  u32 actors = 0;
  u32 actor_values = 0;
  u32 actors_with_values = 0;   // references carrying a value table of their own
  u32 actor_values_unnamed = 0; // rows whose place in the value enumeration is unproven
  u32 actor_perks = 0;          // perks pushed at the sink
  u32 actors_with_perks = 0;    // references carrying a perk array
  u32 actor_faction_ranks = 0;
  u32 actor_levels = 0;       // NPC level, resolved through its level-mult form
  u32 actor_ai_profiles = 0;  // aggression/confidence/energy/morality/mood
  u32 faction_reactions = 0;
  u32 faction_infamy = 0;     // crime factions carrying an infamy count
  u32 dialogue_said = 0;      // INFO records already spoken
  u32 cells_visited = 0;      // cells carrying world/local map exploration
  u32 map_markers = 0;        // named locations the save has found
  u32 map_markers_travel = 0; // of those, the ones that are travel destinations
  u32 references_moved = 0;
  u32 references_enabled = 0;
  u32 references_disabled = 0;
  u32 inventories = 0;       // containers and actors whose contents were pushed
  u32 inventory_items = 0;   // item stacks inside them

  // Change forms that decoded but whose id could not be remapped, so nothing
  // was applied for them.
  u32 refused = 0;
  // Change forms layer 2 declined to decode (an unsupported version, or a
  // record type with no decoder).
  u32 undecoded = 0;

  // What the save carries and this layer deliberately does not push at the
  // sink, either because the payload group is not decoded or because no engine
  // system owns it. Reported so the gap is visible instead of silent.
  // Cells carrying an owner the save changed. The only CELL group this layer
  // steps over rather than reads; the detach time and the exterior coordinates
  // beside it are decoded (see CellChange).
  u32 cells_owned = 0;
  // Inventories layer 2 could only read a prefix of, so what is there is not
  // the container's real contents and none of it is applied.
  u32 inventories_incomplete = 0;
  // Base forms the save invented (player enchantments, brewed potions). They
  // exist only in the save's own tables, so nothing in the load order can be
  // pointed at them and the items that name them are dropped.
  u32 created_forms = 0;
  u32 inventory_items_created = 0;
  // References the save spawned (0xFFxxxxxx REFR/ACHR change forms). Nothing
  // can carry their ids over, so they reach the sink under a handle of their
  // own; the payload names the base form they are an instance of.
  u32 created_references = 0;
  u32 created_references_with_base = 0;
  u32 created_references_spawned = 0;
  // Spawned references the save had disabled or deleted. Nothing in the load
  // order can name them, so no script can ever bring one back: they are dropped
  // rather than pushed at the sink as entities that could never be seen.
  u32 created_references_inert = 0;
};

// Finds the player's reference in the save and remaps its parent. False when
// the save has no player record, when the record carries no transform, or when
// the parent does not remap.
bool FindPlayerPlacement(const SaveFile& save, const FormRemap& remap, PlayerPlacement* out);

// Walks every change form once, in an order that puts state a later record may
// depend on first: globals, then quests, then actor and faction state, then the
// references themselves.
void ApplySave(const SaveFile& save, const FormRemap& remap, SaveSink& sink, SaveApplyStats* stats);

// The actor value name for a skill slot in an NPC_ record's skill array, or an
// empty view when the game's layout is not established. Skyrim's order is the
// 18 skills as the Creation Kit writes them; Fallout 4 reuses the group for a
// different set that has not been validated here, so it yields nothing.
base::StringRef ActorSkillName(SaveFormat format, u32 index);

// The name behind an ActorValueEntry's index. Only the values whose position in
// the game's enumeration is established are named; the rest yield nothing, so a
// row this does not know is dropped rather than applied to the wrong value.
base::StringRef ActorValueName(SaveFormat format, u32 index);

}  // namespace rx::bethesda

#endif  // COMPONENTS_BETHESDA_SAVEGAME_APPLY_H
