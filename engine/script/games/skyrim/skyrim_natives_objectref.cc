#include "script/games/skyrim/skyrim_native_state.h"
#include "script/games/skyrim/skyrim_natives_ext.h"

namespace rec::script::skyrim {

using papyrus::ObjectRef;
using papyrus::Value;
using papyrus::VirtualMachine;
using ext::Args;
using ext::ArgB;
using ext::ArgF;
using ext::ArgI;
using ext::ArgO;
using ext::ArgS;
using ext::Resolve;
namespace st = state;

void RegisterObjectRefExtra(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  // Pure engine commands with no observable script state: wired no-ops until the
  // animation, physics, translation and ragdoll subsystems exist.
  auto noop = [](VirtualMachine&, ObjectRef, Args&) { return Value(); };
  for (const char* fn :
       {"AddDependentAnimatedObjectReference", "AddInventoryEventFilter", "AddToMap",
        "ApplyHavokImpulse", "CreateDetectionEvent", "DamageObject", "DisableNoWait", "DropObject",
        "EnableFastTravel", "EnableNoWait", "ForceAddRagdollToWorld", "ForceRemoveRagdollFromWorld",
        "InterruptCast", "KnockAreaEffect", "MoveToInteractionLocation", "MoveToMyEditorLocation",
        "MoveToNode", "PlayAnimation", "PlayAnimationAndWait", "PlayGamebryoAnimation",
        "PlayImpactEffect", "PlaySyncedAnimationAndWaitSS", "PlaySyncedAnimationSS",
        "PlayTerrainEffect", "ProcessTrapHit", "PushActorAway", "RemoveAllInventoryEventFilters",
        "RemoveAllStolenItems", "RemoveDependentAnimatedObjectReference", "RemoveInventoryEventFilter",
        "Say", "SendStealAlarm", "SetActorCause", "SetContainerAllowStolenItems", "SetNoFavorAllowed",
        "SplineTranslateTo", "SplineTranslateToRefNode", "StopTranslation", "TetherToHorse",
        "TranslateTo", "WaitForAnimationEvent"})
    reg.Register("ObjectReference", fn, noop);

  // PlaceActorAtMe makes a new actor; with no spawn path yet it yields None.
  reg.Register("ObjectReference", "PlaceActorAtMe",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Object(ObjectRef{}); });

  // Cell/location/world getters with no data source yet resolve to None; these
  // (and the fixed-value queries below) await their subsystems.
  auto none_query = [](VirtualMachine&, ObjectRef, Args&) { return Value::Object(ObjectRef{}); };
  for (const char* fn : {"GetCurrentLocation", "GetCurrentScene", "GetEditorLocation", "GetKey",
                         "GetNthLinkedRef", "GetVoiceType", "GetWorldSpace"})
    reg.Register("ObjectReference", fn, none_query);

  // Linked ref and parent cell read the placed REFR record. GetLinkedRef takes an
  // optional keyword to pick among several links.
  reg.Register("ObjectReference", "GetLinkedRef", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    return Value::Object(Resolve(bindings).GetLinkedRef(self, ArgO(a, 0)));
  });
  reg.Register("ObjectReference", "GetParentCell", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Object(Resolve(bindings).GetParentCell(self));
  });

  // Boolean queries with no backing state: the neutral false keeps script
  // branches that test them behaving.
  auto false_query = [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); };
  for (const char* fn : {"CanFastTravelToMarker", "HasEffectKeyword", "HasNode", "HasRefType",
                         "IsActivateChild", "IsDeleted", "IsFurnitureInUse", "IsFurnitureMarkerInUse",
                         "IsInDialogueWithPlayer", "IsLockBroken", "IsMapMarkerVisible"})
    reg.Register("ObjectReference", fn, false_query);

  // Fixed-value queries: no dimensions or destruction model yet.
  auto zero_f = [](VirtualMachine&, ObjectRef, Args&) { return Value::Float(0.0f); };
  for (const char* fn : {"GetHeadingAngle", "GetHeight", "GetLength", "GetMass", "GetWidth"})
    reg.Register("ObjectReference", fn, zero_f);
  reg.Register("ObjectReference", "GetItemHealthPercent",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Float(1.0f); });
  reg.Register("ObjectReference", "GetCurrentDestructionStage",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Int(0); });
  reg.Register("ObjectReference", "GetTriggerObjectCount",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Int(0); });
  reg.Register("ObjectReference", "CalculateEncounterLevel",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Int(1); });

  // Angle: SetAngle stores the three Euler floats GetAngleX/Y/Z read back.
  reg.Register("ObjectReference", "SetAngle", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetFloat(self, "angleX", ArgF(a, 0));
    st::SetFloat(self, "angleY", ArgF(a, 1));
    st::SetFloat(self, "angleZ", ArgF(a, 2));
    return Value();
  });
  reg.Register("ObjectReference", "GetAngleX", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Float(st::GetFloat(self, "angleX"));
  });
  reg.Register("ObjectReference", "GetAngleY", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Float(st::GetFloat(self, "angleY"));
  });
  reg.Register("ObjectReference", "GetAngleZ", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Float(st::GetFloat(self, "angleZ"));
  });

  // Havok motion type: stored only, there is no getter.
  reg.Register("ObjectReference", "SetMotionType", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetInt(self, "motionType", ArgI(a, 0));
    return Value();
  });

  // Destruction state: SetDestroyed marks the flag, ClearDestruction clears it.
  reg.Register("ObjectReference", "SetDestroyed", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetFlag(self, "destroyed", ArgB(a, 0, true));
    return Value();
  });
  reg.Register("ObjectReference", "ClearDestruction", [](VirtualMachine&, ObjectRef self, Args&) {
    st::SetFlag(self, "destroyed", false);
    return Value();
  });

  // Activation block flag, round-tripped through the shared store.
  reg.Register("ObjectReference", "BlockActivation", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetFlag(self, "activationBlocked", ArgB(a, 0, true));
    return Value();
  });
  reg.Register("ObjectReference", "IsActivationBlocked", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(st::GetFlag(self, "activationBlocked"));
  });

  // Friendly-hit immunity flag.
  reg.Register("ObjectReference", "IgnoreFriendlyHits", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetFlag(self, "ignoreFriendlyHits", ArgB(a, 0, true));
    return Value();
  });
  reg.Register("ObjectReference", "IsIgnoringFriendlyHits",
               [](VirtualMachine&, ObjectRef self, Args&) {
                 return Value::Bool(st::GetFlag(self, "ignoreFriendlyHits"));
               });

  // Ownership: actor and faction owner refs round-trip through the store.
  reg.Register("ObjectReference", "SetActorOwner", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetRef(self, "actorOwner", ArgO(a, 0));
    return Value();
  });
  reg.Register("ObjectReference", "GetActorOwner", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Object(st::GetRef(self, "actorOwner"));
  });
  reg.Register("ObjectReference", "SetFactionOwner", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetRef(self, "factionOwner", ArgO(a, 0));
    return Value();
  });
  reg.Register("ObjectReference", "GetFactionOwner", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Object(st::GetRef(self, "factionOwner"));
  });

  // Animation graph variables, keyed by the variable name per type.
  reg.Register("ObjectReference", "SetAnimationVariableBool",
               [](VirtualMachine&, ObjectRef self, Args& a) {
                 st::SetFlag(self, "animB_" + ArgS(a, 0), ArgB(a, 1, false));
                 return Value();
               });
  reg.Register("ObjectReference", "GetAnimationVariableBool",
               [](VirtualMachine&, ObjectRef self, Args& a) {
                 return Value::Bool(st::GetFlag(self, "animB_" + ArgS(a, 0)));
               });
  reg.Register("ObjectReference", "SetAnimationVariableFloat",
               [](VirtualMachine&, ObjectRef self, Args& a) {
                 st::SetFloat(self, "animF_" + ArgS(a, 0), ArgF(a, 1));
                 return Value();
               });
  reg.Register("ObjectReference", "GetAnimationVariableFloat",
               [](VirtualMachine&, ObjectRef self, Args& a) {
                 return Value::Float(st::GetFloat(self, "animF_" + ArgS(a, 0)));
               });
  reg.Register("ObjectReference", "SetAnimationVariableInt",
               [](VirtualMachine&, ObjectRef self, Args& a) {
                 st::SetInt(self, "animI_" + ArgS(a, 0), ArgI(a, 1));
                 return Value();
               });
  reg.Register("ObjectReference", "GetAnimationVariableInt",
               [](VirtualMachine&, ObjectRef self, Args& a) {
                 return Value::Int(st::GetInt(self, "animI_" + ArgS(a, 0)));
               });
}

}  // namespace rec::script::skyrim
