#include "script/games/skyrim/skyrim_native_state.h"
#include "script/games/skyrim/skyrim_natives_ext.h"

namespace rec::script::skyrim {

using papyrus::ObjectRef;
using papyrus::Value;
using papyrus::VirtualMachine;
using ext::Args;
using ext::ArgB; using ext::ArgF; using ext::ArgI; using ext::ArgO; using ext::ArgS;
using ext::Resolve;
namespace st = state;

void RegisterActorExtra(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  // Flag set/is pairs round-trip through the shared state store, so a script that
  // toggles a state then reads it back sees what it wrote even with no subsystem.
  reg.Register("Actor", "SetGhost", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetFlag(self, "ghost", ArgB(a, 0, true));
    return Value();
  });
  reg.Register("Actor", "IsGhost", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(st::GetFlag(self, "ghost"));
  });
  reg.Register("Actor", "SetPlayerTeammate", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetFlag(self, "teammate", ArgB(a, 0, true));
    return Value();
  });
  reg.Register("Actor", "IsPlayerTeammate", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(st::GetFlag(self, "teammate"));
  });
  reg.Register("Actor", "SetUnconscious", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetFlag(self, "unconscious", ArgB(a, 0, true));
    return Value();
  });
  reg.Register("Actor", "IsUnconscious", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(st::GetFlag(self, "unconscious"));
  });
  reg.Register("Actor", "SetBribed", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetFlag(self, "bribed", ArgB(a, 0, true));
    return Value();
  });
  reg.Register("Actor", "IsBribed", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(st::GetFlag(self, "bribed"));
  });
  reg.Register("Actor", "SetDoingFavor", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetFlag(self, "doingFavor", ArgB(a, 0, true));
    return Value();
  });
  reg.Register("Actor", "IsDoingFavor", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(st::GetFlag(self, "doingFavor"));
  });
  reg.Register("Actor", "SetIntimidated", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetFlag(self, "intimidated", ArgB(a, 0, true));
    return Value();
  });
  reg.Register("Actor", "IsIntimidated", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(st::GetFlag(self, "intimidated"));
  });
  reg.Register("Actor", "SetAlert", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetFlag(self, "alert", ArgB(a, 0, true));
    return Value();
  });
  reg.Register("Actor", "IsAlerted", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(st::GetFlag(self, "alert"));
  });
  // SetAllowFlying and its Ex variant share one gate; the extra Ex args (crash
  // behaviour) have no flight subsystem to feed yet.
  reg.Register("Actor", "SetAllowFlying", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetFlag(self, "allowFlying", ArgB(a, 0, true));
    return Value();
  });
  reg.Register("Actor", "SetAllowFlyingEx", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetFlag(self, "allowFlying", ArgB(a, 0, true));
    return Value();
  });
  reg.Register("Actor", "IsAllowedToFly", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(st::GetFlag(self, "allowFlying"));
  });
  reg.Register("Actor", "SetPlayerControls", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetFlag(self, "playerControls", ArgB(a, 0, true));
    return Value();
  });
  reg.Register("Actor", "GetPlayerControls", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(st::GetFlag(self, "playerControls"));
  });
  // ClearArrested lowers the arrest flag IsArrested reads.
  reg.Register("Actor", "ClearArrested", [](VirtualMachine&, ObjectRef self, Args&) {
    st::SetFlag(self, "arrested", false);
    return Value();
  });
  reg.Register("Actor", "IsArrested", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(st::GetFlag(self, "arrested"));
  });
  // DrawWeapon raises the drawn flag; IsWeaponDrawn reads it. There is no holster
  // counterpart in this batch, so it stays raised until something else clears it.
  reg.Register("Actor", "DrawWeapon", [](VirtualMachine&, ObjectRef self, Args&) {
    st::SetFlag(self, "weaponDrawn", true);
    return Value();
  });
  reg.Register("Actor", "IsWeaponDrawn", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(st::GetFlag(self, "weaponDrawn"));
  });
  reg.Register("Actor", "StartSneaking", [](VirtualMachine&, ObjectRef self, Args&) {
    st::SetFlag(self, "sneaking", true);
    return Value();
  });

  // Scalar and ref state, stored so the matching getter (or a later read) sees it.
  reg.Register("Actor", "SetAlpha", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetFloat(self, "alpha", ArgF(a, 0));
    return Value();
  });
  reg.Register("Actor", "SetVoiceRecoveryTime", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetFloat(self, "voiceRecovery", ArgF(a, 0));
    return Value();
  });
  reg.Register("Actor", "GetVoiceRecoveryTime", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Float(st::GetFloat(self, "voiceRecovery"));
  });
  reg.Register("Actor", "SetCrimeFaction", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetRef(self, "crimeFaction", ArgO(a, 0));
    return Value();
  });
  reg.Register("Actor", "GetCrimeFaction", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Object(st::GetRef(self, "crimeFaction"));
  });
  reg.Register("Actor", "SetForcedLandingMarker", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetRef(self, "landingMarker", ArgO(a, 0));
    return Value();
  });
  reg.Register("Actor", "GetForcedLandingMarker", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Object(st::GetRef(self, "landingMarker"));
  });

  // Collection natives back onto member sets. AddSpell lives in the core binding,
  // so HasSpell only sees spells added through this batch's tracking key.
  reg.Register("Actor", "AddPerk", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::AddMember(self, "perks", ArgO(a, 0));
    return Value();
  });
  reg.Register("Actor", "RemovePerk", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::RemoveMember(self, "perks", ArgO(a, 0));
    return Value();
  });
  reg.Register("Actor", "HasPerk", [](VirtualMachine&, ObjectRef self, Args& a) {
    return Value::Bool(st::HasMember(self, "perks", ArgO(a, 0)));
  });
  reg.Register("Actor", "AddShout", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::AddMember(self, "shouts", ArgO(a, 0));
    return Value::Bool(true);
  });
  reg.Register("Actor", "RemoveShout", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::RemoveMember(self, "shouts", ArgO(a, 0));
    return Value::Bool(true);
  });
  reg.Register("Actor", "RemoveSpell", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::RemoveMember(self, "spells", ArgO(a, 0));
    return Value::Bool(true);
  });
  reg.Register("Actor", "DispelSpell", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::RemoveMember(self, "spells", ArgO(a, 0));
    return Value::Bool(true);
  });
  reg.Register("Actor", "HasSpell", [](VirtualMachine&, ObjectRef self, Args& a) {
    return Value::Bool(st::HasMember(self, "spells", ArgO(a, 0)));
  });
  // No member set is exposed to wipe wholesale, so these clear nothing observable.
  reg.Register("Actor", "DispelAllSpells", [](VirtualMachine&, ObjectRef, Args&) { return Value(); });
  reg.Register("Actor", "RemoveFromAllFactions",
               [](VirtualMachine&, ObjectRef, Args&) { return Value(); });

  // Equipped-slot refs: the equip/unequip natives write the slot key the matching
  // getter reads. Source 0 is the left hand, anything else the right.
  auto spell_key = [](i32 source) { return source == 0 ? "spellLeft" : "spellRight"; };
  reg.Register("Actor", "GetEquippedWeapon", [](VirtualMachine&, ObjectRef self, Args& a) {
    return Value::Object(st::GetRef(self, ArgB(a, 0, false) ? "weaponLeft" : "weaponRight"));
  });
  reg.Register("Actor", "GetEquippedShield", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Object(st::GetRef(self, "shield"));
  });
  reg.Register("Actor", "GetEquippedShout", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Object(st::GetRef(self, "shout"));
  });
  reg.Register("Actor", "GetEquippedSpell", [spell_key](VirtualMachine&, ObjectRef self, Args& a) {
    return Value::Object(st::GetRef(self, spell_key(ArgI(a, 0))));
  });
  reg.Register("Actor", "EquipShout", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetRef(self, "shout", ArgO(a, 0));
    return Value();
  });
  reg.Register("Actor", "UnequipShout", [](VirtualMachine&, ObjectRef self, Args&) {
    st::SetRef(self, "shout", ObjectRef{});
    return Value();
  });
  reg.Register("Actor", "EquipSpell", [spell_key](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetRef(self, spell_key(ArgI(a, 1)), ArgO(a, 0));
    return Value();
  });
  reg.Register("Actor", "UnequipSpell", [spell_key](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetRef(self, spell_key(ArgI(a, 1)), ObjectRef{});
    return Value();
  });

  // Getters an existing binding can answer.
  reg.Register("Actor", "Resurrect", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    Resolve(bindings).Resurrect(self);
    return Value();
  });
  reg.Register("Actor", "IsEssential", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(Resolve(bindings).IsEssential(self));
  });
  // Hostility derives from a deeply negative relationship rank (foe/archnemesis).
  reg.Register("Actor", "IsHostileToActor", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    return Value::Bool(Resolve(bindings).GetRelationshipRank(self, ArgO(a, 0)) <= -3);
  });
  reg.Register("Actor", "ModFactionRank", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    auto& b = Resolve(bindings);
    ObjectRef faction = ArgO(a, 0);
    b.SetFactionRank(self, faction, b.GetFactionRank(self, faction) + ArgI(a, 1));
    return Value();
  });
  // Relationship aggregates have no per-pair table to scan, so report neutral.
  reg.Register("Actor", "GetHighestRelationshipRank",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Int(0); });
  reg.Register("Actor", "GetLowestRelationshipRank",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Int(0); });

  // Queries with no subsystem behind them yet return the neutral of their Papyrus
  // return type; they become real once detection, flight, mounts, etc. exist.
  reg.Register("Actor", "CanFlyHere",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Actor", "Dismount",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Actor", "GetBribeAmount",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Int(0); });
  reg.Register("Actor", "GetCombatState",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Int(0); });
  reg.Register("Actor", "GetCurrentPackage",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Object(ObjectRef{}); });
  reg.Register("Actor", "GetDialogueTarget",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Object(ObjectRef{}); });
  reg.Register("Actor", "GetEquippedArmorInSlot",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Object(ObjectRef{}); });
  reg.Register("Actor", "GetEquippedItemType",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Int(0); });
  reg.Register("Actor", "GetFactionReaction",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Int(0); });
  reg.Register("Actor", "GetFlyingState",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Int(0); });
  reg.Register("Actor", "GetGoldAmount",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Int(0); });
  reg.Register("Actor", "GetKiller",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Object(ObjectRef{}); });
  reg.Register("Actor", "GetLeveledActorBase",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Object(ObjectRef{}); });
  reg.Register("Actor", "GetLightLevel",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Float(1.0f); });
  reg.Register("Actor", "GetNoBleedoutRecovery",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Actor", "GetSitState",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Int(0); });
  reg.Register("Actor", "GetSleepState",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Int(0); });
  reg.Register("Actor", "GetWarmthRating",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Float(0.0f); });
  reg.Register("Actor", "HasAssociation",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Actor", "HasFamilyRelationship",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Actor", "HasLOS",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Actor", "HasMagicEffect",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Actor", "HasMagicEffectWithKeyword",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Actor", "HasParentRelationship",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Actor", "IsAlarmed",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Actor", "IsArrestingTarget",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Actor", "IsBeingRidden",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Actor", "IsCommandedActor",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Actor", "IsDetectedBy",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Actor", "IsEquipped",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Actor", "IsFlying",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Actor", "IsGuard",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Actor", "IsInKillMove",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Actor", "IsOnMount",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Actor", "IsOverEncumbered",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Actor", "IsPlayersLastRiddenHorse",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Actor", "IsRunning",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Actor", "IsSprinting",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Actor", "IsTrespassing",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Actor", "PathToReference",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Actor", "TrapSoul",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Actor", "WillIntimidateSucceed",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Actor", "WornHasKeyword",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });

  // Pure commands with no observable script state: wired no-ops until the
  // animation, AI-package, crime and menu subsystems they drive exist.
  auto noop = [](VirtualMachine&, ObjectRef, Args&) { return Value(); };
  reg.Register("Actor", "AllowBleedoutDialogue", noop);
  reg.Register("Actor", "AllowPCDialogue", noop);
  reg.Register("Actor", "AttachAshPile", noop);
  reg.Register("Actor", "ClearExpressionOverride", noop);
  reg.Register("Actor", "ClearExtraArrows", noop);
  reg.Register("Actor", "ClearForcedMovement", noop);
  reg.Register("Actor", "ClearKeepOffsetFromActor", noop);
  reg.Register("Actor", "ClearLookAt", noop);
  reg.Register("Actor", "DoCombatSpellApply", noop);
  reg.Register("Actor", "EndDeferredKill", noop);
  reg.Register("Actor", "ForceMovementDirection", noop);
  reg.Register("Actor", "ForceMovementDirectionRamp", noop);
  reg.Register("Actor", "ForceMovementRotationSpeed", noop);
  reg.Register("Actor", "ForceMovementRotationSpeedRamp", noop);
  reg.Register("Actor", "ForceMovementSpeed", noop);
  reg.Register("Actor", "ForceMovementSpeedRamp", noop);
  reg.Register("Actor", "ForceTargetAngle", noop);
  reg.Register("Actor", "ForceTargetDirection", noop);
  reg.Register("Actor", "ForceTargetSpeed", noop);
  reg.Register("Actor", "KeepOffsetFromActor", noop);
  reg.Register("Actor", "MoveToPackageLocation", noop);
  reg.Register("Actor", "OpenInventory", noop);
  reg.Register("Actor", "PlayIdle", noop);
  reg.Register("Actor", "PlayIdleWithTarget", noop);
  reg.Register("Actor", "PlaySubGraphAnimation", noop);
  reg.Register("Actor", "ResetHealthAndLimbs", noop);
  reg.Register("Actor", "SendAssaultAlarm", noop);
  reg.Register("Actor", "SendLycanthropyStateChanged", noop);
  reg.Register("Actor", "SendTrespassAlarm", noop);
  reg.Register("Actor", "SendVampirismStateChanged", noop);
  reg.Register("Actor", "SetAttackActorOnSight", noop);
  reg.Register("Actor", "SetCriticalStage", noop);
  reg.Register("Actor", "SetExpressionOverride", noop);
  reg.Register("Actor", "SetEyeTexture", noop);
  reg.Register("Actor", "SetHeadTracking", noop);
  reg.Register("Actor", "SetLookAt", noop);
  reg.Register("Actor", "SetNotShowOnStealthMeter", noop);
  reg.Register("Actor", "SetOutfit", noop);
  reg.Register("Actor", "SetPlayerResistingArrest", noop);
  reg.Register("Actor", "SetRace", noop);
  reg.Register("Actor", "SetSubGraphFloatVariable", noop);
  reg.Register("Actor", "SetVehicle", noop);
  reg.Register("Actor", "ShowBarterMenu", noop);
  reg.Register("Actor", "ShowGiftMenu", noop);
  reg.Register("Actor", "StartCannibal", noop);
  reg.Register("Actor", "StartDeferredKill", noop);
  reg.Register("Actor", "StartVampireFeed", noop);
  reg.Register("Actor", "UnequipAll", noop);
  reg.Register("Actor", "UnequipItem", noop);
  reg.Register("Actor", "UnequipItemSlot", noop);
  reg.Register("Actor", "UnLockOwnedDoorsInCell", noop);
}

}  // namespace rec::script::skyrim
