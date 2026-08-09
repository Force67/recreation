#include "components/bethesda/savegame_apply.h"

#include <base/algorithm.h>

namespace rx::bethesda {
namespace {

constexpr u16 kNotLoaded = 0xffff;

// The runtime quest flags a QUST change form carries in its first byte. Only
// these two say anything the journal shows; the rest are authoring flags that
// travel with the record. Measured against the reference save: every quest with
// kQuestFlagCompleted set also carries stage data, and the start-game-enabled
// quests that are still up read 0x11 (enabled + started).
constexpr u8 kQuestFlagCompleted = 0x02;
constexpr u8 kQuestFlagStarted = 0x10;

constexpr u32 kObjectiveDisplayed = 0x01;
constexpr u32 kObjectiveCompleted = 0x02;

// Skyrim's NPC_ skill array, in the order the Creation Kit writes it. These are
// the actor value names the engine already knows (see sdk/Engine/ActorValue.cs).
// Where the 18 skills start in the game's actor value enumeration, which is the
// order kSkyrimSkills is in.
constexpr u32 kSkyrimFirstSkillValue = 6;

constexpr const char* kSkyrimSkills[kActorSkillCount] = {
    "OneHanded",  "TwoHanded",   "Marksman",    "Block",    "Smithing",    "HeavyArmor",
    "LightArmor", "Pickpocket",  "Lockpicking", "Sneak",    "Alchemy",     "Speechcraft",
    "Alteration", "Conjuration", "Destruction", "Illusion", "Restoration", "Enchanting",
};

void Charge(RemapCounters& counters, FormRemap::Refusal reason) {
  switch (reason) {
    case FormRemap::Refusal::kNone:
      ++counters.mapped;
      return;
    case FormRemap::Refusal::kMissingPlugin:
      ++counters.missing_plugin;
      return;
    case FormRemap::Refusal::kOutOfRange:
      ++counters.out_of_range;
      return;
    case FormRemap::Refusal::kCreated:
      ++counters.created;
      return;
  }
}

// Every id that reaches a sink goes through here, so the tally in the stats is
// the tally of what was actually attempted.
bool MapCounted(const FormRemap& remap, u32 id, GlobalFormId* out, SaveApplyStats* stats) {
  FormRemap::Refusal reason = FormRemap::Refusal::kNone;
  const bool ok = remap.Map(id, out, &reason);
  Charge(stats->forms, reason);
  return ok;
}

void ApplyQuest(const ChangeForm& form,
                const SaveFile& save,
                const FormRemap& remap,
                SaveSink& sink,
                SaveApplyStats* stats) {
  QuestChange quest;
  if (!DecodeQuest(form, quest)) {
    ++stats->undecoded;
    return;
  }
  GlobalFormId id;
  if (!MapCounted(remap, form.form_id, &id, stats)) {
    ++stats->refused;
    return;
  }
  ++stats->quests;

  // The journal only ever moves forward, so the stages go in ascending order
  // and the last one that ran is where the quest stands.
  base::Vector<i32> done;
  for (const QuestStageState& stage : quest.stages) {
    if (stage.flags & 1)
      done.push_back(static_cast<i32>(stage.stage));
  }
  base::Sort(done.begin(), done.end());
  for (i32 stage : done) {
    sink.SetQuestStageDone(id, stage);
    ++stats->quest_stages;
  }
  sink.SetQuestState(id, done.empty() ? 0 : done.back(),
                     (quest.quest_flags & kQuestFlagStarted) != 0,
                     (quest.quest_flags & kQuestFlagCompleted) != 0);

  for (const QuestObjectiveState& objective : quest.objectives) {
    sink.SetQuestObjective(id, static_cast<i32>(objective.index),
                           (objective.state & kObjectiveDisplayed) != 0,
                           (objective.state & kObjectiveCompleted) != 0);
    ++stats->quest_objectives;
  }

  for (const QuestAliasFill& fill : quest.alias_fills) {
    const u32 raw = ResolveChangeRef(
        fill.ref, base::Span<const u32>(save.form_ids.data(), save.form_ids.size()));
    GlobalFormId ref;
    if (raw == 0 || !MapCounted(remap, raw, &ref, stats))
      continue;
    sink.FillQuestAlias(id, fill.alias_id, ref);
    ++stats->quest_aliases;
  }
}

void ApplyActorBase(const ChangeForm& form,
                    const SaveFile& save,
                    const FormRemap& remap,
                    SaveSink& sink,
                    SaveApplyStats* stats) {
  ActorBaseChange actor;
  if (!DecodeActorBase(form, actor)) {
    ++stats->undecoded;
    return;
  }
  GlobalFormId id;
  if (!MapCounted(remap, form.form_id, &id, stats)) {
    ++stats->refused;
    return;
  }
  ++stats->actors;

  if (actor.has_stats) {
    sink.SetActorLevel(id, ResolveActorLevel(actor.base_flags, actor.level, actor.calc_min_level,
                                             actor.calc_max_level, save.player_level));
    ++stats->actor_levels;
  }
  if (actor.has_ai) {
    sink.SetActorAi(id, ActorAi{actor.aggression, actor.confidence, actor.energy, actor.morality,
                                actor.mood, actor.assistance});
    ++stats->actor_ai_profiles;
  }

  if (actor.has_skills) {
    sink.SetActorValue(id, "Health", static_cast<f32>(actor.health));
    sink.SetActorValue(id, "Magicka", static_cast<f32>(actor.magicka));
    sink.SetActorValue(id, "Stamina", static_cast<f32>(actor.stamina));
    stats->actor_values += 3;
    for (u32 i = 0; i < kActorSkillCount; ++i) {
      const base::StringRef name = ActorSkillName(save.format, i);
      if (name.empty())
        break;
      // The offsets are the levelled-up part of a skill; the value the game
      // reads is the sum.
      sink.SetActorValue(
          id, name, static_cast<f32>(actor.skills[i]) + static_cast<f32>(actor.skill_offsets[i]));
      ++stats->actor_values;
    }
  }

  for (const ActorFactionRank& entry : actor.factions) {
    const u32 raw = ResolveChangeRef(
        entry.faction, base::Span<const u32>(save.form_ids.data(), save.form_ids.size()));
    GlobalFormId faction;
    if (raw == 0 || !MapCounted(remap, raw, &faction, stats))
      continue;
    sink.SetActorFactionRank(id, faction, entry.rank);
    ++stats->actor_faction_ranks;
  }
}

void ApplyFaction(const ChangeForm& form,
                  const SaveFile& save,
                  const FormRemap& remap,
                  SaveSink& sink,
                  SaveApplyStats* stats) {
  FactionChange faction;
  if (!DecodeFaction(form, faction)) {
    ++stats->undecoded;
    return;
  }
  GlobalFormId id;
  if (!MapCounted(remap, form.form_id, &id, stats)) {
    ++stats->refused;
    return;
  }
  if (faction.has_crime) {
    sink.SetFactionInfamy(id, faction.infamy_violent, faction.infamy_non_violent);
    ++stats->faction_infamy;
  }

  for (const FactionReaction& reaction : faction.reactions) {
    const u32 raw = ResolveChangeRef(
        reaction.faction, base::Span<const u32>(save.form_ids.data(), save.form_ids.size()));
    GlobalFormId other;
    if (raw == 0 || !MapCounted(remap, raw, &other, stats))
      continue;
    sink.SetFactionReaction(id, other, static_cast<i32>(reaction.combat_reaction));
    ++stats->faction_reactions;
  }
}

// A 0xFFxxxxxx REFR/ACHR: the save's own reference. Its id means nothing in the
// running game, so it is handed over under a synthetic one and the sink places
// it from the base form and transform the payload carries.
void ApplyCreatedReference(const ChangeForm& form,
                           const ReferenceChange& ref,
                           const SaveFile& save,
                           const FormRemap& remap,
                           SaveSink& sink,
                           SaveApplyStats* stats) {
  ++stats->created_references;
  GlobalFormId base;
  if (ref.base_object.none() || !remap.MapRef(ref.base_object, save, &base))
    return;
  ++stats->created_references_with_base;

  // A reference the save had disabled or deleted has no handle anywhere in the
  // load order, so nothing in the running game could ever enable it again.
  if (ref.has_form_flags &&
      (ref.form_flags & (kFormFlagDeleted | kFormFlagInitiallyDisabled)) != 0) {
    ++stats->created_references_inert;
    return;
  }
  GlobalFormId parent;
  if (!ref.moved || !remap.MapRef(ref.parent, save, &parent))
    return;

  const GlobalFormId id{kCreatedReferencePlugin, form.form_id & 0xffffff};
  sink.SpawnCreatedReference(id, base, parent, ref.position, ref.rotation,
                             ref.has_scale ? ref.scale : 1.0f,
                             form.type == ChangeFormType::kAchr);
  ++stats->created_references_spawned;
}

void ApplyReference(const ChangeForm& form,
                    const SaveFile& save,
                    const FormRemap& remap,
                    SaveSink& sink,
                    SaveApplyStats* stats,
                    bool move_it = true) {
  ReferenceChange ref;
  if (!DecodeReference(form, ref)) {
    ++stats->undecoded;
    return;
  }
  GlobalFormId id;
  if (!MapCounted(remap, form.form_id, &id, stats)) {
    ++stats->refused;
    // A reference the save spawned has no record in any plugin, so its id
    // cannot be carried over. What it does carry is the base form it is an
    // instance of and where it stands, which is enough to put it back.
    if ((form.form_id >> 24) == 0xff)
      ApplyCreatedReference(form, ref, save, remap, sink, stats);
    return;
  }

  // An actor's own values: what it levelled its way to, keyed by the reference
  // rather than the base form because no other actor sharing that base has them.
  for (const ActorValueEntry& value : ref.actor_values) {
    const base::StringRef name = ActorValueName(save.format, value.index);
    if (name.empty()) {
      ++stats->actor_values_unnamed;
      continue;
    }
    sink.SetReferenceActorValue(id, name, value.value);
    ++stats->actor_values;
  }
  if (!ref.actor_values.empty())
    ++stats->actors_with_values;

  for (const ActorPerk& perk : ref.perks) {
    GlobalFormId form;
    if (!remap.MapRef(perk.perk, save, &form))
      continue;
    sink.AddReferencePerk(id, form, perk.rank);
    ++stats->actor_perks;
  }
  if (!ref.perks.empty())
    ++stats->actors_with_perks;

  if (!ref.inventory.empty() && !ref.inventory_complete)
    ++stats->inventories_incomplete;
  if (!ref.inventory.empty() && ref.inventory_complete) {
    ++stats->inventories;
    for (const InventoryItem& entry : ref.inventory) {
      const u32 raw = ResolveChangeRef(
          entry.item, base::Span<const u32>(save.form_ids.data(), save.form_ids.size()));
      // An item the save invented has no record anywhere in the load order, so
      // it is counted rather than handed over as some other form.
      if ((raw >> 24) == 0xff) {
        ++stats->inventory_items_created;
        continue;
      }
      GlobalFormId item;
      if (raw == 0 || !MapCounted(remap, raw, &item, stats))
        continue;
      sink.AddContainerItem(id, item, entry.count, entry.equipped || entry.equipped_left);
      ++stats->inventory_items;
    }
  }

  if (ref.moved && move_it) {
    // Without its parent the transform is a position in an unknown space, so a
    // reference whose cell did not survive the remap is left where the records
    // put it rather than moved to coordinates that mean nothing.
    const u32 raw = ResolveChangeRef(
        ref.parent, base::Span<const u32>(save.form_ids.data(), save.form_ids.size()));
    GlobalFormId parent;
    if (raw != 0 && MapCounted(remap, raw, &parent, stats)) {
      sink.MoveReference(id, parent, ref.position, ref.rotation);
      ++stats->references_moved;
    }
  }

  if (ref.has_map_marker) {
    const bool visible = (ref.map_marker_flags & kMapMarkerVisible) != 0;
    const bool can_travel = (ref.map_marker_flags & kMapMarkerCanTravelTo) != 0;
    sink.SetMapMarker(id, visible, can_travel);
    ++stats->map_markers;
    if (can_travel)
      ++stats->map_markers_travel;
  }

  if (ref.has_form_flags) {
    const bool enabled = (ref.form_flags & kFormFlagInitiallyDisabled) == 0;
    sink.SetReferenceEnabled(id, enabled);
    if (enabled)
      ++stats->references_enabled;
    else
      ++stats->references_disabled;
  }
}

}  // namespace

void FormRemap::Build(const SaveFile& save,
                      const base::Function<u16(const base::String&)>& runtime_index) {
  for (u16& slot : mod_)
    slot = kNotLoaded;
  light_.clear();
  missing_plugins_.clear();

  const size_t count = base::Min<size_t>(save.plugins.size(), 256);
  plugin_count_ = static_cast<u8>(base::Min<size_t>(count, 255));
  for (size_t i = 0; i < count; ++i) {
    mod_[i] = runtime_index(save.plugins[i]);
    if (mod_[i] == kNotLoaded)
      missing_plugins_.push_back(save.plugins[i]);
  }

  light_.resize(save.light_plugins.size());
  for (size_t i = 0; i < save.light_plugins.size(); ++i) {
    light_[i] = runtime_index(save.light_plugins[i]);
    if (light_[i] == kNotLoaded)
      missing_plugins_.push_back(save.light_plugins[i]);
  }
  built_ = true;
}

bool FormRemap::Map(u32 save_form_id, GlobalFormId* out, Refusal* reason) const {
  const auto refuse = [reason](Refusal why) {
    if (reason)
      *reason = why;
    return false;
  };
  if (reason)
    *reason = Refusal::kNone;

  const u8 high = static_cast<u8>(save_form_id >> 24);
  if (high == 0xff)
    return refuse(Refusal::kCreated);
  if (high == 0xfe) {
    const u32 slot = (save_form_id >> 12) & 0xfff;
    if (slot >= light_.size())
      return refuse(Refusal::kOutOfRange);
    if (light_[slot] == kNotLoaded)
      return refuse(Refusal::kMissingPlugin);
    // A light plugin holds no load order slot of its own at runtime, so its
    // forms land at that plugin's index with the 12 bit local id.
    *out = GlobalFormId{light_[slot], save_form_id & 0xfff};
    return true;
  }
  if (high >= plugin_count_)
    return refuse(Refusal::kOutOfRange);
  if (mod_[high] == kNotLoaded)
    return refuse(Refusal::kMissingPlugin);
  *out = GlobalFormId{mod_[high], save_form_id & 0xffffff};
  return true;
}

bool FormRemap::MapRef(ChangeRef ref, const SaveFile& save, GlobalFormId* out) const {
  const u32 raw =
      ResolveChangeRef(ref, base::Span<const u32>(save.form_ids.data(), save.form_ids.size()));
  if (raw == 0)
    return false;
  return Map(raw, out);
}

base::StringRef ActorSkillName(SaveFormat format, u32 index) {
  if (index >= kActorSkillCount)
    return {};
  switch (format) {
    case SaveFormat::kSkyrimLe:
    case SaveFormat::kSkyrimSe:
      return kSkyrimSkills[index];
    default:
      return {};
  }
}

base::StringRef ActorValueName(SaveFormat format, u32 index) {
  if (format != SaveFormat::kSkyrimLe && format != SaveFormat::kSkyrimSe)
    return {};
  // The 18 skills sit at 6 through 23, which is where an actor's value table
  // and the NPC_ skill array meet.
  if (index >= kSkyrimFirstSkillValue && index < kSkyrimFirstSkillValue + kActorSkillCount)
    return kSkyrimSkills[index - kSkyrimFirstSkillValue];
  switch (index) {
    case 0: return "Aggression";
    case 1: return "Confidence";
    case 2: return "Energy";
    case 3: return "Morality";
    case 4: return "Mood";
    case 5: return "Assistance";
    case 24: return "Health";
    case 25: return "Magicka";
    case 26: return "Stamina";
    case 27: return "HealRate";
    case 28: return "MagickaRate";
    case 29: return "StaminaRate";
    case 30: return "SpeedMult";
    case 31: return "InventoryWeight";
    case 32: return "CarryWeight";
    case 33: return "CritChance";
    case 34: return "MeleeDamage";
    case 35: return "UnarmedDamage";
    case 36: return "DamageResist";
    case 37: return "PoisonResist";
    case 38: return "ResistFire";
    case 39: return "ResistShock";
    case 40: return "ResistFrost";
    case 41: return "ResistMagic";
    case 42: return "ResistDisease";
    // Past the resistances the enumeration is not established here, and a value
    // applied to the wrong name is worse than one not applied at all.
    default: return {};
  }
}

bool FindPlayerPlacement(const SaveFile& save, const FormRemap& remap, PlayerPlacement* out) {
  for (const ChangeForm& form : save.change_forms) {
    if (form.form_id != kPlayerFormId)
      continue;
    if (form.type != ChangeFormType::kAchr && form.type != ChangeFormType::kRefr)
      continue;
    ReferenceChange ref;
    if (!DecodeReference(form, ref) || !ref.moved)
      return false;
    PlayerPlacement placement;
    if (!remap.MapRef(ref.parent, save, &placement.parent))
      return false;
    for (u32 i = 0; i < 3; ++i) {
      placement.position[i] = ref.position[i];
      placement.rotation[i] = ref.rotation[i];
    }
    placement.valid = true;
    *out = placement;
    return true;
  }
  return false;
}

void ApplySave(const SaveFile& save,
               const FormRemap& remap,
               SaveSink& sink,
               SaveApplyStats* stats) {
  SaveApplyStats local;
  if (!stats)
    stats = &local;
  if (!remap.built())
    return;

  stats->created_forms = static_cast<u32>(save.created_forms.size());

  // Globals first: they are the cheapest state and the one quest conditions and
  // the world clock read, so nothing else should observe the authored values.
  for (const base::Pair<u32, f32>& global : save.globals) {
    GlobalFormId id;
    if (!MapCounted(remap, global.first, &id, stats))
      continue;
    sink.SetGlobal(id, global.second);
    ++stats->globals;
  }

  // Then the record types, each in its own pass so the order is the order of
  // the passes and not of whatever the file happens to list first: quests
  // before the actors and references their aliases point at.
  for (const ChangeForm& form : save.change_forms) {
    if (form.type == ChangeFormType::kQust)
      ApplyQuest(form, save, remap, sink, stats);
  }
  for (const ChangeForm& form : save.change_forms) {
    if (form.type == ChangeFormType::kNpc)
      ApplyActorBase(form, save, remap, sink, stats);
  }
  for (const ChangeForm& form : save.change_forms) {
    if (form.type == ChangeFormType::kFact)
      ApplyFaction(form, save, remap, sink, stats);
  }
  for (const ChangeForm& form : save.change_forms) {
    if (form.type != ChangeFormType::kRefr && form.type != ChangeFormType::kAchr)
      continue;
    // The player's record goes through the same walk as any other reference,
    // for its values and its pack, but not for its position: where the player
    // stands decides which cell the world streams, so the boot path takes that
    // on its own out of FindPlayerPlacement.
    ApplyReference(form, save, remap, sink, stats, form.form_id != kPlayerFormId);
  }

  // Map exploration and spoken dialogue last: neither depends on anything above
  // and both are counted per record rather than per group.
  for (const ChangeForm& form : save.change_forms) {
    if (form.type == ChangeFormType::kCell) {
      CellChange cell;
      if (!DecodeCell(form, cell)) {
        ++stats->undecoded;
        continue;
      }
      if (form.flags & kCellChangeOwnership)
        ++stats->cells_owned;
      if (cell.visited.empty())
        continue;
      GlobalFormId id;
      if (!MapCounted(remap, form.form_id, &id, stats)) {
        ++stats->refused;
        continue;
      }
      sink.SetCellVisited(id, base::Span<const CellVisitedGrid>(cell.visited.data(),
                                                                cell.visited.size()));
      stats->cells_visited += static_cast<u32>(cell.visited.size());
    } else if (form.type == ChangeFormType::kInfo) {
      DialogueInfoChange info;
      if (!DecodeDialogueInfo(form, info) || !info.said)
        continue;
      GlobalFormId id;
      if (!MapCounted(remap, form.form_id, &id, stats)) {
        ++stats->refused;
        continue;
      }
      sink.SetDialogueSaid(id);
      ++stats->dialogue_said;
    }
  }
}

}  // namespace rx::bethesda
