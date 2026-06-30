#ifndef RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_NATIVES_H_
#define RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_NATIVES_H_

#include <string>

#include "core/types.h"
#include "script/papyrus/native.h"
#include "script/papyrus/value.h"

namespace rec::script::skyrim {

// Everything the Skyrim native surface needs from the engine. The engine
// implements the slices it supports; the rest keep the defaults here, so a
// partially wired engine still runs scripts (they just see neutral values).
// The guest never sees an engine type, only this interface, which keeps the two
// worlds decoupled. A Fallout binding would be a separate interface paired with
// a separate native table.
class SkyrimBindings {
 public:
  virtual ~SkyrimBindings() = default;

  // Game
  virtual papyrus::ObjectRef GetPlayer() { return {}; }
  // Looks up a form by its runtime (load-order) form id.
  virtual papyrus::ObjectRef GetForm(u32 form_id) { return {}; }

  // Form
  virtual u32 GetFormId(papyrus::ObjectRef form) { return 0; }
  virtual i32 GetFormType(papyrus::ObjectRef form) { return 0; }
  // Whether a bare reference is of `type_name` ("Actor", "ObjectReference", ...),
  // used by the VM to resolve `obj as Type` casts for refs with no script.
  virtual bool RefIsType(papyrus::ObjectRef ref, const std::string& type_name) { return false; }
  virtual std::string GetName(papyrus::ObjectRef form) { return ""; }
  virtual bool HasKeyword(papyrus::ObjectRef form, papyrus::ObjectRef keyword) { return false; }
  // Enumerates a form's keywords (KWDA), so mods can categorise items by every
  // keyword they carry. GetKeywordCount parses them into a cache and returns the
  // count; GetNthKeyword reads that cache by index.
  virtual i32 GetKeywordCount(papyrus::ObjectRef form) { return 0; }
  virtual papyrus::ObjectRef GetNthKeyword(i32 index) { return {}; }
  // Item weight and gold value, read from the form's record. Supported for the
  // common inventory item types (weapons, armor, misc, ingredients, soul gems,
  // keys, potions); other forms report 0.
  virtual f32 GetWeight(papyrus::ObjectRef form) { return 0; }
  virtual i32 GetGoldValue(papyrus::ObjectRef form) { return 0; }
  // Base physical damage of a weapon form (WEAP); 0 for non-weapons.
  virtual i32 GetWeaponDamage(papyrus::ObjectRef weapon) { return 0; }
  // Base armor rating of an armor form (ARMO); 0 for non-armor.
  virtual f32 GetArmorRating(papyrus::ObjectRef armor) { return 0; }
  // The enchantment on a weapon or armor form (its EITM); None if unenchanted.
  virtual papyrus::ObjectRef GetEnchantment(papyrus::ObjectRef item) { return {}; }

  // A soul gem's soul state (SLGM): the soul currently held (SOUL, 0 empty .. 5
  // grand) and the capacity it can hold (SLCP, its size class). Both 0 for a
  // non-soul-gem. Managed code uses the soul to power enchanting or recharging.
  virtual i32 GetSoulGemSoul(papyrus::ObjectRef gem) { return 0; }
  virtual i32 GetSoulGemCapacity(papyrus::ObjectRef gem) { return 0; }

  // The ingredient a harvestable flora (FLOR) produces, from its PFIG; None for
  // non-flora. The form passed is the flora base, not a placed reference.
  virtual papyrus::ObjectRef GetHarvestIngredient(papyrus::ObjectRef flora) { return {}; }

  // Books (BOOK) that teach: GetBookSpell returns the spell a spell tome grants
  // (None otherwise), GetBookSkill the actor-value name a skill book raises (empty
  // otherwise). The two are mutually exclusive in the record's DATA flags.
  virtual papyrus::ObjectRef GetBookSpell(papyrus::ObjectRef book) { return {}; }
  virtual std::string GetBookSkill(papyrus::ObjectRef book) { return ""; }

  // The magic effects of an ingredient or potion/food (INGR or ALCH), shared by
  // alchemy (matching effects across ingredients) and consumables (applying a
  // potion's effects). Call GetMagicEffectCount first: it parses the effects
  // (EFID/EFIT) into a cache and returns the count; the GetNthMagicEffect*
  // accessors then read that cache by index.
  virtual i32 GetMagicEffectCount(papyrus::ObjectRef item) { return 0; }
  virtual papyrus::ObjectRef GetNthMagicEffectId(i32 index) { return {}; }
  virtual f32 GetNthMagicEffectMagnitude(i32 index) { return 0; }
  virtual i32 GetNthMagicEffectDuration(i32 index) { return 0; }

  // A shout's words (SHOU): up to three, each a word of power with the spell it
  // casts and its recovery time. Call GetShoutWordCount first to parse them into a
  // cache; the GetNthShout* accessors then read it by index, mirroring the magic
  // effect accessors above.
  virtual i32 GetShoutWordCount(papyrus::ObjectRef shout) { return 0; }
  virtual papyrus::ObjectRef GetNthShoutWord(i32 index) { return {}; }
  virtual papyrus::ObjectRef GetNthShoutSpell(i32 index) { return {}; }
  virtual f32 GetNthShoutRecoveryTime(i32 index) { return 0; }

  // Properties of a magic effect (MGEF): the actor-value name it modifies (empty
  // for values not modelled) and whether it is detrimental (damage rather than
  // restore/fortify). Together with the per-effect duration these let managed code
  // apply a consumable's effects.
  virtual std::string GetMagicEffectActorValue(papyrus::ObjectRef effect) { return ""; }
  virtual bool GetMagicEffectDetrimental(papyrus::ObjectRef effect) { return false; }
  virtual f32 GetMagicEffectBaseCost(papyrus::ObjectRef effect) { return 0.0f; }

  // A spell's SPIT fields: the magicka cost, the type (0 spell, 2 power, 3 lesser
  // power, 4 ability, 5 poison, 11 voice), the cast type (0 constant effect,
  // 1 fire-and-forget, 2 concentration) and the delivery (0 self, 1 touch, 2 aimed,
  // 3 target actor, 4 target location). Let managed code model casting and powers.
  virtual i32 GetSpellCost(papyrus::ObjectRef spell) { return 0; }
  virtual i32 GetSpellType(papyrus::ObjectRef spell) { return 0; }
  virtual i32 GetSpellCastType(papyrus::ObjectRef spell) { return 0; }
  virtual i32 GetSpellDelivery(papyrus::ObjectRef spell) { return 0; }

  // Constructible-object recipes (COBJ): the data behind smithing, cooking,
  // tempering and tanning. GetRecipeCount parses every recipe once into a cache
  // and returns the total; the GetNthRecipe* accessors read it by index, inputs
  // addressed by (recipe, input). All form ids come back as resolved handles.
  virtual i32 GetRecipeCount() { return 0; }
  virtual papyrus::ObjectRef GetNthRecipeOutput(i32 recipe) { return {}; }
  virtual i32 GetNthRecipeOutputQuantity(i32 recipe) { return 0; }
  virtual papyrus::ObjectRef GetNthRecipeWorkbench(i32 recipe) { return {}; }
  virtual i32 GetNthRecipeInputCount(i32 recipe) { return 0; }
  virtual papyrus::ObjectRef GetNthRecipeInput(i32 recipe, i32 input) { return {}; }
  virtual i32 GetNthRecipeInputQuantity(i32 recipe, i32 input) { return 0; }

  // Leveled item lists (LVLI), the data behind loot, vendor stock and drops.
  // GetLeveledListCount parses the list into a cache and returns its entry count
  // (0 for a non-LVLI form); the other accessors read that cache, the chance of
  // nothing (LVLD), the flags (LVLF), and each entry's form/level/count (LVLO).
  // The managed resolver does the level-gating and random picks.
  virtual i32 GetLeveledListCount(papyrus::ObjectRef list) { return 0; }
  virtual i32 GetLeveledChanceNone() { return 0; }
  virtual i32 GetLeveledFlags() { return 0; }
  virtual papyrus::ObjectRef GetNthLeveledForm(i32 index) { return {}; }
  virtual i32 GetNthLeveledLevel(i32 index) { return 0; }
  virtual i32 GetNthLeveledCount(i32 index) { return 0; }

  // Sound playback, routed to the engine audio system when one is wired. PlaySound
  // resolves a SOUN/SNDR form to its asset and plays it, returning a voice id (0 on
  // failure); StopSoundInstance and SetSoundInstanceVolume act on that voice.
  // SetSoundCategoryVolume scales a category (the master bus stands in for now).
  virtual i32 PlaySound(papyrus::ObjectRef sound, papyrus::ObjectRef source) { return 0; }
  virtual void StopSoundInstance(i32 instance) {}
  virtual void SetSoundInstanceVolume(i32 instance, f32 volume) {}
  virtual void SetSoundCategoryVolume(papyrus::ObjectRef category, f32 volume) {}

  // A form list (FLST record): its authored entries (LNAM), in order. Call
  // GetFormListSize first to parse the list into a cache and return its count,
  // then GetNthListForm reads that cache by index. FormList.GetAt/Find/HasForm
  // read the base list this way, on top of any runtime-added forms.
  virtual i32 GetFormListSize(papyrus::ObjectRef list) { return 0; }
  virtual papyrus::ObjectRef GetNthListForm(i32 index) { return {}; }

  // ActorBase (NPC_ record data).
  virtual i32 GetSex(papyrus::ObjectRef actor_base) { return 0; }  // 0 male, 1 female
  virtual bool IsUnique(papyrus::ObjectRef actor_base) { return false; }
  virtual bool IsEssential(papyrus::ObjectRef actor_base) { return false; }
  virtual papyrus::ObjectRef GetRace(papyrus::ObjectRef actor_base) { return {}; }

  // A race's innate spells (RACE SPLO): its abilities and powers. GetRaceSpellCount
  // parses them into a cache and returns the count; GetNthRaceSpell reads it by
  // index. Managed code applies the abilities and uses the powers.
  virtual i32 GetRaceSpellCount(papyrus::ObjectRef race) { return 0; }
  virtual papyrus::ObjectRef GetNthRaceSpell(i32 index) { return {}; }

  // A race's starting skill bonuses (RACE DATA): the actor-value name of each
  // skill it favours and the bonus. GetRaceSkillBonusCount parses them into a
  // cache and returns the count; the GetNth accessors read it. Character creation.
  virtual i32 GetRaceSkillBonusCount(papyrus::ObjectRef race) { return 0; }
  virtual std::string GetNthRaceSkillBonusSkill(i32 index) { return ""; }
  virtual i32 GetNthRaceSkillBonusValue(i32 index) { return 0; }

  // Proximity over a per-frame position snapshot: references within `radius`
  // (game units) of `center`. GetNearbyRefs computes and caches the set and
  // returns its size; GetNthNearbyRef reads that cached result by index. Both run
  // on the guest thread; the snapshot is refreshed by the runtime.
  virtual i32 GetNearbyRefs(papyrus::ObjectRef center, f32 radius) { return 0; }
  virtual papyrus::ObjectRef GetNthNearbyRef(i32 index) { return {}; }
  virtual f32 GetNthNearbyDistance(i32 index) { return 0; }

  // ObjectReference spatial state (engine transform / ECS).
  virtual f32 GetPositionX(papyrus::ObjectRef ref) { return 0; }
  virtual f32 GetPositionY(papyrus::ObjectRef ref) { return 0; }
  virtual f32 GetPositionZ(papyrus::ObjectRef ref) { return 0; }
  virtual void SetPosition(papyrus::ObjectRef ref, f32 x, f32 y, f32 z) {}
  virtual f32 GetDistance(papyrus::ObjectRef a, papyrus::ObjectRef b) { return 0; }
  virtual void MoveTo(papyrus::ObjectRef ref, papyrus::ObjectRef target) {}
  virtual void SetEnabled(papyrus::ObjectRef ref, bool enabled) {}
  virtual papyrus::ObjectRef GetBaseObject(papyrus::ObjectRef ref) { return {}; }
  // The reference a quest alias is filled with (ReferenceAlias.GetReference).
  virtual papyrus::ObjectRef AliasReference(papyrus::ObjectRef alias) { return {}; }
  // Runtime alias fill (ReferenceAlias.ForceRefTo / Clear): a script-set override
  // that takes precedence over the authored fill rule until cleared.
  virtual void AliasForceRefTo(papyrus::ObjectRef alias, papyrus::ObjectRef ref) {}
  virtual void AliasClear(papyrus::ObjectRef alias) {}
  // Fills the quest's "Find Matching Reference" aliases (ALFA/ALRT) from a
  // location's LCSR table: each alias binds to a distinct placed reference whose
  // LocationRefType matches the alias's. This is how the Civil War siege binds
  // its Attacker*/Defender*/Marker* slots to a fort's placed soldiers and markers.
  // Returns the number of aliases filled.
  virtual int FillFindMatchingAliases(papyrus::ObjectRef quest, papyrus::ObjectRef location) {
    return 0;
  }
  // A quest location alias (ALLS): GetAliasLocation reads the location it is filled
  // with, ForceAliasLocation sets it (ReferenceAlias/LocationAlias.GetLocation /
  // ForceLocationTo). The Civil War siege's Alias_Fort is set this way to the fort
  // being attacked, which scopes its find-matching reference aliases.
  virtual papyrus::ObjectRef GetAliasLocation(papyrus::ObjectRef alias) { return {}; }
  virtual void ForceAliasLocation(papyrus::ObjectRef alias, papyrus::ObjectRef location) {}
  // Keyword data: a float associated with a keyword on a form. The Civil War
  // campaign stores each hold's owning faction as keyword data on its Location
  // (CWScript.GetOwner reads Location.GetKeywordData(CWOwner)), which gates the
  // siege's attacker/defender classification.
  virtual f32 GetKeywordData(papyrus::ObjectRef form, papyrus::ObjectRef keyword) { return 0; }
  virtual void SetKeywordData(papyrus::ObjectRef form, papyrus::ObjectRef keyword, f32 value) {}

  // Cell data (CELL record).
  virtual bool IsInterior(papyrus::ObjectRef cell) { return false; }
  virtual f32 GetCellWaterLevel(papyrus::ObjectRef cell) { return 0; }
  virtual bool IsDisabled(papyrus::ObjectRef ref) { return false; }
  virtual i32 GetItemCount(papyrus::ObjectRef container, papyrus::ObjectRef item) { return 0; }
  virtual void AddItem(papyrus::ObjectRef container, papyrus::ObjectRef item, i32 count) {}
  virtual void RemoveItem(papyrus::ObjectRef container, papyrus::ObjectRef item, i32 count) {}
  // Inventory enumeration: the number of distinct item forms a container holds,
  // and the form at an index in [0, GetNumItems). The order is stable only while
  // the inventory is unchanged, so a listing loop must not add or remove items.
  virtual i32 GetNumItems(papyrus::ObjectRef container) { return 0; }
  virtual papyrus::ObjectRef GetNthForm(papyrus::ObjectRef container, i32 index) { return {}; }

  // A placed reference's linked ref (XLKR), optionally filtered by keyword, and the
  // interior cell that contains it. Both read the REFR record; None when absent.
  virtual papyrus::ObjectRef GetLinkedRef(papyrus::ObjectRef ref, papyrus::ObjectRef keyword) {
    return {};
  }
  virtual papyrus::ObjectRef GetParentCell(papyrus::ObjectRef ref) { return {}; }
  virtual void Activate(papyrus::ObjectRef target, papyrus::ObjectRef activator) {}
  virtual void Delete(papyrus::ObjectRef ref) {}
  virtual papyrus::ObjectRef PlaceAtMe(papyrus::ObjectRef where, papyrus::ObjectRef base,
                                       i32 count) {
    return {};
  }
  virtual f32 GetScale(papyrus::ObjectRef ref) { return 1.0f; }
  virtual void SetScale(papyrus::ObjectRef ref, f32 scale) {}
  // Lock and door state (new system).
  virtual bool IsLocked(papyrus::ObjectRef ref) { return false; }
  virtual void SetLocked(papyrus::ObjectRef ref, bool locked) {}
  virtual i32 GetLockLevel(papyrus::ObjectRef ref) { return 0; }
  virtual void SetLockLevel(papyrus::ObjectRef ref, i32 level) {}
  virtual i32 GetOpenState(papyrus::ObjectRef ref) { return 3; }  // 1 open, 3 closed
  virtual void SetOpen(papyrus::ObjectRef ref, bool open) {}

  // Actor values and state. The value has a permanent base and a damageable
  // current; percentage is current/base.
  virtual f32 GetActorValue(papyrus::ObjectRef actor, const std::string& av) { return 0; }
  virtual f32 GetBaseActorValue(papyrus::ObjectRef actor, const std::string& av) { return 0; }
  virtual f32 GetActorValuePercentage(papyrus::ObjectRef actor, const std::string& av) {
    return 1.0f;
  }
  virtual void SetActorValue(papyrus::ObjectRef actor, const std::string& av, f32 value) {}
  virtual void ForceActorValue(papyrus::ObjectRef actor, const std::string& av, f32 value) {}
  virtual void ModActorValue(papyrus::ObjectRef actor, const std::string& av, f32 delta) {}
  virtual void RestoreActorValue(papyrus::ObjectRef actor, const std::string& av, f32 amount) {}
  virtual i32 GetLevel(papyrus::ObjectRef actor) { return 1; }
  virtual bool IsDead(papyrus::ObjectRef actor) { return false; }
  // Brings a dead actor back: restores its health and clears its downed state so
  // it renders upright and can fight again (ObjectReference.Reset, the Civil War
  // reinforcement respawn). A no-op on anything that did not die.
  virtual void Resurrect(papyrus::ObjectRef actor) {}
  virtual bool IsInCombat(papyrus::ObjectRef actor) { return false; }
  virtual bool IsSneaking(papyrus::ObjectRef actor) { return false; }
  virtual papyrus::ObjectRef GetCombatTarget(papyrus::ObjectRef actor) { return {}; }
  // Relationship rank between two actors (-4 archnemesis .. 4 lover). The Civil
  // War governance scripts set it and read it back; a stored symmetric value
  // round-trips so those branches behave.
  virtual void SetRelationshipRank(papyrus::ObjectRef a, papyrus::ObjectRef b, i32 rank) {}
  virtual i32 GetRelationshipRank(papyrus::ObjectRef a, papyrus::ObjectRef b) { return 0; }
  // Enters/leaves melee combat. StartCombat makes `actor` fight `target` (the
  // engine's combat driver then closes the distance and swings); StopCombat
  // withdraws it. Quest and AI scripts drive battles through these.
  virtual void StartCombat(papyrus::ObjectRef actor, papyrus::ObjectRef target) {}
  virtual void StopCombat(papyrus::ObjectRef actor) {}
  // Make an actor follow the player (KeepOffsetFromActor with the player as the
  // target) or stop (ClearKeepOffsetFromActor). Drives the NPC follow behaviour.
  virtual void SetActorFollowing(papyrus::ObjectRef actor, bool follow) {}
  virtual void EquipItem(papyrus::ObjectRef actor, papyrus::ObjectRef item) {}
  virtual void AddSpell(papyrus::ObjectRef actor, papyrus::ObjectRef spell) {}

  // Enumerates an actor's authored faction memberships (its NPC_ record's SNAM
  // entries). GetFactionCount parses them into a cache and returns the count;
  // GetNthFaction / GetNthFactionRank read it by index. Runtime changes
  // (Add/Remove/SetFactionRank) override these but do not change the listing.
  virtual i32 GetFactionCount(papyrus::ObjectRef actor) { return 0; }
  virtual papyrus::ObjectRef GetNthFaction(i32 index) { return {}; }
  virtual i32 GetNthFactionRank(i32 index) { return 0; }

  // Faction membership (actor side) and faction-wide reaction / crime state.
  virtual i32 GetFactionRank(papyrus::ObjectRef actor, papyrus::ObjectRef faction) { return -2; }
  virtual void SetFactionRank(papyrus::ObjectRef actor, papyrus::ObjectRef faction, i32 rank) {}
  virtual bool IsInFaction(papyrus::ObjectRef actor, papyrus::ObjectRef faction) { return false; }
  virtual void AddToFaction(papyrus::ObjectRef actor, papyrus::ObjectRef faction) {}
  virtual void RemoveFromFaction(papyrus::ObjectRef actor, papyrus::ObjectRef faction) {}
  virtual i32 GetReaction(papyrus::ObjectRef faction, papyrus::ObjectRef other) { return 0; }
  // A faction's DATA flags (FACT record). Bit 0x40 marks a crime faction (it
  // tracks the player's bounty). Lets managed code find the hold an actor reports
  // crimes to.
  virtual i32 GetFactionFlags(papyrus::ObjectRef faction) { return 0; }
  virtual void SetReaction(papyrus::ObjectRef faction, papyrus::ObjectRef other, i32 reaction) {}
  virtual i32 GetCrimeGold(papyrus::ObjectRef faction) { return 0; }
  virtual void SetCrimeGold(papyrus::ObjectRef faction, i32 gold) {}
  virtual void ModCrimeGold(papyrus::ObjectRef faction, i32 delta) {}

  // Quests (new in-engine state system).
  virtual i32 GetStage(papyrus::ObjectRef quest) { return 0; }
  virtual void SetStage(papyrus::ObjectRef quest, i32 stage) {}
  virtual bool GetStageDone(papyrus::ObjectRef quest, i32 stage) { return false; }
  // Journal (CNAM) text of the most recently set stage, empty for a control
  // stage that carries no log entry. Lets soft logic surface journal updates.
  virtual std::string GetJournalEntry(papyrus::ObjectRef quest) { return ""; }
  virtual bool IsRunning(papyrus::ObjectRef quest) { return false; }
  virtual void StartQuest(papyrus::ObjectRef quest) {}
  virtual void StopQuest(papyrus::ObjectRef quest) {}
  virtual void ResetQuest(papyrus::ObjectRef quest) {}
  virtual bool IsQuestActive(papyrus::ObjectRef quest) { return false; }
  virtual void SetQuestActive(papyrus::ObjectRef quest, bool active) {}
  virtual void SetObjectiveDisplayed(papyrus::ObjectRef quest, i32 objective, bool displayed) {}
  virtual void SetObjectiveCompleted(papyrus::ObjectRef quest, i32 objective, bool completed) {}
  virtual bool IsObjectiveDisplayed(papyrus::ObjectRef quest, i32 objective) { return false; }
  virtual bool IsObjectiveCompleted(papyrus::ObjectRef quest, i32 objective) { return false; }

  // Scenes: a SCEN fragment runs Self.GetOwningQuest().SetStage(N), so the scene
  // object must resolve to its owning quest. Returns None for an unknown scene.
  virtual papyrus::ObjectRef SceneOwningQuest(papyrus::ObjectRef scene) { return papyrus::ObjectRef{0}; }
  // The quest a dialogue INFO belongs to (TopicInfo.GetOwningQuest), so a TIF_
  // fragment's GetOwningQuest().SetStage advances the right quest.
  virtual papyrus::ObjectRef InfoOwningQuest(papyrus::ObjectRef info) { return papyrus::ObjectRef{0}; }
  // Scene.Start/ForceStart begin playing a scene (a quest fragment calls this);
  // Stop ends it; IsPlaying queries it. Default no-ops for the neutral bindings.
  virtual void SceneStart(papyrus::ObjectRef scene) {}
  virtual void SceneStop(papyrus::ObjectRef scene) {}
  virtual bool SceneIsPlaying(papyrus::ObjectRef scene) { return false; }

  // Player control gate (new system). Categories: 0 movement, 1 fighting,
  // 2 cam-switch, 3 looking, 4 sneaking, 5 menu, 6 activate, 7 journal,
  // 8 fast-travel. All enabled by default; cutscene scripts toggle them.
  static constexpr i32 kControlCount = 9;
  virtual void SetPlayerControl(i32 category, bool enabled) {}
  virtual bool IsPlayerControlEnabled(i32 category) { return true; }

  // Global variables: initial value from the GLOB record, overridable.
  virtual f32 GetGlobalValue(papyrus::ObjectRef global) { return 0; }
  virtual void SetGlobalValue(papyrus::ObjectRef global, f32 value) {}

  // Game settings and time.
  virtual f32 GetGameSettingFloat(const std::string& name) { return 0; }
  virtual f32 GetRealHoursPassed() { return 0; }
  virtual f32 GetCurrentGameTime() { return 0; }  // days since game start

  // Managed HUD gauges: a named persistent bar a gameplay system (oxygen,
  // radiation, ...) pushes each tick, the counterpart to Debug.Notification's
  // transient toast. `fraction` is clamped 0..1, `color` is packed rgba8 (0 lets
  // the HUD pick one). The engine snapshots them onto the HUD each frame.
  // Clearing one removes its bar. Default no-ops for the neutral bindings.
  virtual void SetHudGauge(const std::string& id, f32 fraction, const std::string& label,
                           u32 color) {}
  virtual void ClearHudGauge(const std::string& id) {}

  // War-map state, pushed by managed gameplay (the Civil War campaign): each hold
  // by index with its name and owner (0 neutral, 1 Imperial, 2 Stormcloak), plus
  // the overall Imperial-controlled fraction. The engine snapshots it onto the
  // toggle-able war-map panel.
  virtual void SetWarHold(i32 index, const std::string& name, i32 owner) {}
  virtual void SetWarProgress(f32 imperial_fraction) {}
};

// Registers the Skyrim Papyrus native surface into reg, bound against bindings
// (pass nullptr to use neutral defaults). Math/Utility globals are fully
// implemented here; engine-touching natives route through bindings.
void RegisterSkyrimNatives(papyrus::NativeRegistry& reg, SkyrimBindings* bindings);

}  // namespace rec::script::skyrim

#endif  // RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_NATIVES_H_
