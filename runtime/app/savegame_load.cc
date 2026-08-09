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
#include "components/script/papyrus/alias_handle.h"
#include "components/script/papyrus/value.h"
#include "core/log.h"
#include "runtime/app/engine.h"
#include "runtime/app/engine_internal.h"

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
// The TESForm flag a record carries when it is placed disabled. The engine
// reads the same bit off the record when it streams a reference in.
constexpr u32 kRecordFlagInitiallyDisabled = 0x800;
// The NPC_ record behind the player reference, "Player" in both games this
// reader covers. The save writes the player's level and skills onto it.
constexpr u32 kPlayerBaseFormId = 0x00000007;

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
                 world::MapDiscovery& map)
      : bindings_(bindings),
        records_(records),
        actor_stats_(actor_stats),
        said_(said),
        map_(map) {}

  void SetGlobal(bethesda::GlobalFormId global, f32 value) override {
    bindings_.SetGlobalValue(Ref(global), value);
  }

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

  void MoveReference(bethesda::GlobalFormId ref,
                     bethesda::GlobalFormId parent,
                     const f32 position[3],
                     const f32 rotation[3]) override {
    if (!records_.Find(ref)) {
      ++unknown_refs_;
      return;
    }
    // Game units in, exactly like the Papyrus SetPosition this shares a store
    // with; the world sink converts. The reference's new cell and its rotation
    // have nowhere to go (see the report in LogTally).
    bindings_.SetPosition(Ref(ref), position[0], position[1], position[2]);
    ++moved_refs_;
  }

  // The save stores container contents as a signed delta against what the
  // records author, so the authored contents have to be in the store before the
  // delta means anything. Seeding them here rather than at streaming time is
  // what makes a looted chest come back empty instead of full.
  void AddContainerItem(bethesda::GlobalFormId container,
                        bethesda::GlobalFormId item,
                        i32 delta,
                        bool equipped) override {
    if (!records_.Find(container)) {
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
    RX_INFO("save: {} actor values, {} faction ranks, {} references toggled, {} moved",
            actor_values_, faction_ranks_, toggled_refs_, moved_refs_);
    RX_INFO(
        "save: {} actor levels and {} ai profiles onto placed actors, {} crime factions carry "
        "infamy, {} dialogue lines already said, {} exterior cells and {} interiors on the map",
        actor_levels_, ai_profiles_, infamy_factions_, said_.size(), visited_cells_,
        map_.VisitedInteriors());
    RX_INFO("save: {} item stacks restored into {} containers seeded from their records",
            inventory_items_, seeded_containers_);
    RX_INFO("save: {} actor values onto references, {} of them the player's own",
            reference_values_, player_values_);
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
  base::UnorderedMap<u64, u64> placed_actors_;  // NPC base -> placed ref, 0 = none
  u32 actor_values_ = 0;
  u32 reference_values_ = 0;
  u32 player_values_ = 0;
  u32 faction_ranks_ = 0;
  u32 toggled_refs_ = 0;
  u32 moved_refs_ = 0;
  u32 actor_levels_ = 0;
  u32 ai_profiles_ = 0;
  u32 infamy_factions_ = 0;
  u32 visited_cells_ = 0;
  u32 unplaced_actors_ = 0;
  u32 unknown_refs_ = 0;
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
                      self->said_topics_, self->map_discovery_);
  // On the guest thread and synchronously: everything below is guest-owned
  // state, and the quest scripts that read it attach right after this returns.
  PlayerReadout readout;
  self->scripts_->guest().Dispatch([&](script::papyrus::VirtualMachine&) {
    bethesda::ApplySave(self->save_->file, self->save_->remap, sink, &stats);
    readout = ReadPlayerBack(*self->script_bindings_);
  });

  RX_INFO(
      "save: applied {} globals, {} quests ({} stages, {} objectives, {} alias fills), {} actors",
      stats.globals, stats.quests, stats.quest_stages, stats.quest_objectives, stats.quest_aliases,
      stats.actors);
  RX_INFO(
      "save: applied {} actor levels, {} ai profiles, {} faction infamy counts, "
      "{} spoken dialogue lines, {} visited map grids",
      stats.actor_levels, stats.actor_ai_profiles, stats.faction_infamy, stats.dialogue_said,
      stats.cells_visited);
  sink.LogTally();
  RX_INFO("save: {} form ids remapped, {} refused (created at runtime), {} change forms dropped",
          stats.forms.mapped, stats.forms.created, stats.refused);
  if (self->clock_)
    RX_INFO("save: resumed at game hour {:.2f} of day {}", self->clock_->hour(),
            self->clock_->game_days());
  RX_INFO("save: applied {} inventories ({} item stacks)", stats.inventories,
          stats.inventory_items);
  // Systems the save carries state for that the engine has nowhere to put yet.
  RX_INFO(
      "save: not applied: {} inventories read only in part, {} item stacks naming a form the save "
      "invented (of {} such forms), {} detached cells",
      stats.inventories_incomplete, stats.inventory_items_created, stats.created_forms,
      stats.cells_detached);
  RX_INFO(
      "save: {} of the dropped change forms are references the save spawned, {} of which name a "
      "base form this load order has; nothing spawns them yet",
      stats.created_references, stats.created_references_with_base);
  // Read straight back out of the live systems rather than out of the decoder,
  // so the line says what the running game believes the player is.
  RX_INFO("save: the player is now level {}, {:.0f}/{:.0f}/{:.0f} health/magicka/stamina, "
          "carrying {:.0f}, one-handed {:.0f} sneak {:.0f} destruction {:.0f}",
          readout.level, readout.health, readout.magicka, readout.stamina, readout.carry_weight,
          readout.one_handed, readout.sneak, readout.destruction);
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
  const Vec3 feet{position[0] * scale, position[2] * scale, -position[1] * scale};
  // Game euler z is the compass yaw, which is already the engine's camera yaw.
  const f32 yaw = self->save_->player.rotation[2];

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
