#include "script/games/skyrim/skyrim_natives.h"

#include <cmath>
#include <numbers>
#include <vector>

#include "core/log.h"

namespace rec::script::skyrim {
namespace {

using papyrus::ObjectRef;
using papyrus::Value;
using papyrus::VirtualMachine;
using Args = std::vector<Value>;

constexpr f64 kDegToRad = std::numbers::pi / 180.0;
constexpr f64 kRadToDeg = 180.0 / std::numbers::pi;

f32 ArgF(const Args& a, size_t i) { return i < a.size() ? a[i].ToFloat() : 0.0f; }
i32 ArgI(const Args& a, size_t i) { return i < a.size() ? a[i].ToInt() : 0; }
bool ArgB(const Args& a, size_t i, bool fallback) { return i < a.size() ? a[i].ToBool() : fallback; }
std::string ArgS(const Args& a, size_t i) { return i < a.size() ? a[i].ToString() : std::string(); }
ObjectRef ArgO(const Args& a, size_t i) { return i < a.size() ? a[i].as_object() : ObjectRef{}; }

// Small deterministic PRNG for Utility.Random*. Determinism makes scripted
// randomness reproducible across runs, which the engine wants for replication.
u64& RngState() {
  static u64 state = 0x9e3779b97f4a7c15ull;
  return state;
}
u32 NextRandom() {
  u64& s = RngState();
  s ^= s << 13;
  s ^= s >> 7;
  s ^= s << 17;
  return static_cast<u32>(s >> 32);
}

SkyrimBindings& Resolve(SkyrimBindings* bindings) {
  static SkyrimBindings kDefault;  // neutral defaults for an unwired engine
  return bindings ? *bindings : kDefault;
}

void RegisterMath(papyrus::NativeRegistry& reg) {
  // Papyrus trigonometry is in degrees, not radians.
  reg.Register("Math", "Sqrt", [](VirtualMachine&, ObjectRef, Args& a) {
    return Value::Float(std::sqrt(ArgF(a, 0)));
  });
  reg.Register("Math", "Abs", [](VirtualMachine&, ObjectRef, Args& a) {
    return Value::Float(std::fabs(ArgF(a, 0)));
  });
  reg.Register("Math", "Pow", [](VirtualMachine&, ObjectRef, Args& a) {
    return Value::Float(std::pow(ArgF(a, 0), ArgF(a, 1)));
  });
  reg.Register("Math", "Floor", [](VirtualMachine&, ObjectRef, Args& a) {
    return Value::Int(static_cast<i32>(std::floor(ArgF(a, 0))));
  });
  reg.Register("Math", "Ceiling", [](VirtualMachine&, ObjectRef, Args& a) {
    return Value::Int(static_cast<i32>(std::ceil(ArgF(a, 0))));
  });
  reg.Register("Math", "Sin", [](VirtualMachine&, ObjectRef, Args& a) {
    return Value::Float(static_cast<f32>(std::sin(ArgF(a, 0) * kDegToRad)));
  });
  reg.Register("Math", "Cos", [](VirtualMachine&, ObjectRef, Args& a) {
    return Value::Float(static_cast<f32>(std::cos(ArgF(a, 0) * kDegToRad)));
  });
  reg.Register("Math", "Tan", [](VirtualMachine&, ObjectRef, Args& a) {
    return Value::Float(static_cast<f32>(std::tan(ArgF(a, 0) * kDegToRad)));
  });
  reg.Register("Math", "Asin", [](VirtualMachine&, ObjectRef, Args& a) {
    return Value::Float(static_cast<f32>(std::asin(ArgF(a, 0)) * kRadToDeg));
  });
  reg.Register("Math", "Acos", [](VirtualMachine&, ObjectRef, Args& a) {
    return Value::Float(static_cast<f32>(std::acos(ArgF(a, 0)) * kRadToDeg));
  });
  reg.Register("Math", "Atan", [](VirtualMachine&, ObjectRef, Args& a) {
    return Value::Float(static_cast<f32>(std::atan(ArgF(a, 0)) * kRadToDeg));
  });
  reg.Register("Math", "Atan2", [](VirtualMachine&, ObjectRef, Args& a) {
    return Value::Float(static_cast<f32>(std::atan2(ArgF(a, 0), ArgF(a, 1)) * kRadToDeg));
  });
  reg.Register("Math", "DegreesToRadians", [](VirtualMachine&, ObjectRef, Args& a) {
    return Value::Float(static_cast<f32>(ArgF(a, 0) * kDegToRad));
  });
  reg.Register("Math", "RadiansToDegrees", [](VirtualMachine&, ObjectRef, Args& a) {
    return Value::Float(static_cast<f32>(ArgF(a, 0) * kRadToDeg));
  });
}

void RegisterDebug(papyrus::NativeRegistry& reg) {
  // Engine-independent diagnostics. Output natives that the guest also binds
  // (Trace, Notification) stay with the guest; these are the value-returning
  // ones a default could answer wrongly.
  reg.Register("Debug", "GetPlatformName",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Str("PC"); });
  reg.Register("Debug", "GetVersionNumber",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Str("1.6.640"); });
  reg.Register("Debug", "TraceStack", [](VirtualMachine&, ObjectRef, Args& a) {
    REC_INFO("[papyrus:stack] {}", a.empty() ? "" : a[0].ToString());
    return Value();
  });
}

void RegisterGameControls(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  // Each query reports a real control category's gate state.
  auto query = [bindings](i32 category) {
    return [bindings, category](VirtualMachine&, ObjectRef, Args&) {
      return Value::Bool(Resolve(bindings).IsPlayerControlEnabled(category));
    };
  };
  reg.Register("Game", "IsMovementControlsEnabled", query(0));
  reg.Register("Game", "IsFightingControlsEnabled", query(1));
  reg.Register("Game", "IsCamSwitchControlsEnabled", query(2));
  reg.Register("Game", "IsLookingControlsEnabled", query(3));
  reg.Register("Game", "IsSneakingControlsEnabled", query(4));
  reg.Register("Game", "IsMenuControlsEnabled", query(5));
  reg.Register("Game", "IsActivateControlsEnabled", query(6));
  reg.Register("Game", "IsJournalControlsEnabled", query(7));
  reg.Register("Game", "IsFastTravelControlsEnabled", query(8));
  reg.Register("Game", "IsFastTravelEnabled", query(8));

  // DisablePlayerControls / EnablePlayerControls toggle categories 0-7; the bool
  // args follow Skyrim's order and defaults.
  auto toggle = [](SkyrimBindings& b, Args& a, bool enable) {
    static constexpr bool kDefault[8] = {true, true, false, true, false, true, true, true};
    for (i32 c = 0; c < 8; ++c)
      if (ArgB(a, c, kDefault[c])) b.SetPlayerControl(c, enable);
  };
  reg.Register("Game", "DisablePlayerControls", [bindings, toggle](VirtualMachine&, ObjectRef, Args& a) {
    toggle(Resolve(bindings), a, false);
    return Value();
  });
  reg.Register("Game", "EnablePlayerControls", [bindings, toggle](VirtualMachine&, ObjectRef, Args& a) {
    toggle(Resolve(bindings), a, true);
    return Value();
  });
  reg.Register("Game", "EnableFastTravel", [bindings](VirtualMachine&, ObjectRef, Args& a) {
    Resolve(bindings).SetPlayerControl(8, ArgB(a, 0, true));
    return Value();
  });

  reg.Register("Game", "GetRealHoursPassed", [bindings](VirtualMachine&, ObjectRef, Args&) {
    return Value::Float(Resolve(bindings).GetRealHoursPassed());
  });
}

void RegisterUtility(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  reg.Register("Utility", "RandomInt", [](VirtualMachine&, ObjectRef, Args& a) {
    i32 lo = a.size() > 0 ? ArgI(a, 0) : 0;
    i32 hi = a.size() > 1 ? ArgI(a, 1) : 100;
    if (hi < lo) std::swap(lo, hi);
    u32 span = static_cast<u32>(hi - lo) + 1;
    return Value::Int(lo + static_cast<i32>(NextRandom() % span));
  });
  reg.Register("Utility", "RandomFloat", [](VirtualMachine&, ObjectRef, Args& a) {
    f32 lo = a.size() > 0 ? ArgF(a, 0) : 0.0f;
    f32 hi = a.size() > 1 ? ArgF(a, 1) : 1.0f;
    f32 t = static_cast<f32>(NextRandom()) / static_cast<f32>(0xffffffffu);
    return Value::Float(lo + (hi - lo) * t);
  });
  reg.Register("Utility", "GetCurrentRealTime", [bindings](VirtualMachine&, ObjectRef, Args&) {
    return Value::Float(Resolve(bindings).GetRealHoursPassed() * 3600.0f);
  });
  // Suspension is not modeled yet: Wait variants return immediately. Scripts
  // keep running; only their pacing differs.
  auto wait = [](VirtualMachine&, ObjectRef, Args&) { return Value(); };
  reg.Register("Utility", "Wait", wait);
  reg.Register("Utility", "WaitMenuMode", wait);
  reg.Register("Utility", "WaitGameTime", wait);
  reg.Register("Utility", "GetCurrentGameTime", [bindings](VirtualMachine&, ObjectRef, Args&) {
    return Value::Float(Resolve(bindings).GetCurrentGameTime());
  });
  reg.Register("Utility", "IsInMenuMode",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
}

void RegisterGameAndForms(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  reg.Register("Game", "GetPlayer", [bindings](VirtualMachine&, ObjectRef, Args&) {
    return Value::Object(Resolve(bindings).GetPlayer());
  });
  reg.Register("Game", "GetForm", [bindings](VirtualMachine&, ObjectRef, Args& a) {
    return Value::Object(Resolve(bindings).GetForm(static_cast<u32>(ArgI(a, 0))));
  });
  reg.Register("Game", "GetGameSettingFloat", [bindings](VirtualMachine&, ObjectRef, Args& a) {
    return Value::Float(Resolve(bindings).GetGameSettingFloat(ArgS(a, 0)));
  });
  reg.Register("Game", "UsingGamepad",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });

  reg.Register("Form", "GetFormID", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Int(static_cast<i32>(Resolve(bindings).GetFormId(self)));
  });
  reg.Register("Form", "GetType", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Int(Resolve(bindings).GetFormType(self));
  });
  reg.Register("Form", "GetName", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Str(Resolve(bindings).GetName(self));
  });
  reg.Register("Form", "HasKeyword", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    return Value::Bool(Resolve(bindings).HasKeyword(self, ArgO(a, 0)));
  });

  // ActorBase (and Actor, which delegates) read the NPC_ record.
  for (const char* type : {"ActorBase", "Actor"}) {
    reg.Register(type, "GetSex", [bindings](VirtualMachine&, ObjectRef self, Args&) {
      return Value::Int(Resolve(bindings).GetSex(self));
    });
    reg.Register(type, "GetRace", [bindings](VirtualMachine&, ObjectRef self, Args&) {
      return Value::Object(Resolve(bindings).GetRace(self));
    });
  }
  reg.Register("ActorBase", "IsUnique", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(Resolve(bindings).IsUnique(self));
  });
  reg.Register("ActorBase", "IsEssential", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(Resolve(bindings).IsEssential(self));
  });
}

void RegisterObjectReference(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  reg.Register("ObjectReference", "GetPositionX", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Float(Resolve(bindings).GetPositionX(self));
  });
  reg.Register("ObjectReference", "GetPositionY", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Float(Resolve(bindings).GetPositionY(self));
  });
  reg.Register("ObjectReference", "GetPositionZ", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Float(Resolve(bindings).GetPositionZ(self));
  });
  reg.Register("ObjectReference", "SetPosition",
               [bindings](VirtualMachine&, ObjectRef self, Args& a) {
                 Resolve(bindings).SetPosition(self, ArgF(a, 0), ArgF(a, 1), ArgF(a, 2));
                 return Value();
               });
  reg.Register("ObjectReference", "GetDistance", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    return Value::Float(Resolve(bindings).GetDistance(self, ArgO(a, 0)));
  });
  reg.Register("ObjectReference", "GetBaseObject", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Object(Resolve(bindings).GetBaseObject(self));
  });
  reg.Register("ObjectReference", "MoveTo", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).MoveTo(self, ArgO(a, 0));
    return Value();
  });
  reg.Register("ObjectReference", "Enable", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    Resolve(bindings).SetEnabled(self, true);
    return Value();
  });
  reg.Register("ObjectReference", "Disable", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    Resolve(bindings).SetEnabled(self, false);
    return Value();
  });
  reg.Register("ObjectReference", "IsDisabled", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(Resolve(bindings).IsDisabled(self));
  });
  reg.Register("ObjectReference", "GetItemCount", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    return Value::Int(Resolve(bindings).GetItemCount(self, ArgO(a, 0)));
  });
  reg.Register("ObjectReference", "AddItem", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).AddItem(self, ArgO(a, 0), a.size() > 1 ? ArgI(a, 1) : 1);
    return Value();
  });
  reg.Register("ObjectReference", "RemoveItem", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).RemoveItem(self, ArgO(a, 0), a.size() > 1 ? ArgI(a, 1) : 1);
    return Value();
  });
  reg.Register("ObjectReference", "GetNumItems", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Int(Resolve(bindings).GetNumItems(self));
  });
  reg.Register("ObjectReference", "GetNthForm", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    return Value::Object(Resolve(bindings).GetNthForm(self, ArgI(a, 0)));
  });
  reg.Register("ObjectReference", "Activate", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).Activate(self, ArgO(a, 0));
    return Value();
  });
  reg.Register("ObjectReference", "Delete", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    Resolve(bindings).Delete(self);
    return Value();
  });
  reg.Register("ObjectReference", "PlaceAtMe", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    return Value::Object(Resolve(bindings).PlaceAtMe(self, ArgO(a, 0), a.size() > 1 ? ArgI(a, 1) : 1));
  });
  reg.Register("ObjectReference", "GetScale", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Float(Resolve(bindings).GetScale(self));
  });
  reg.Register("ObjectReference", "SetScale", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).SetScale(self, ArgF(a, 0));
    return Value();
  });
  // Refs are enabled unless disabled; the neutral "false" would be wrong.
  reg.Register("ObjectReference", "IsEnabled", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(!Resolve(bindings).IsDisabled(self));
  });
  reg.Register("ObjectReference", "Is3DLoaded",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(true); });
  // A quest alias resolves to its filled reference. ReferenceAlias.GetReference
  // is the native; GetActorReference/GetActorRef/GetRef are wrappers in script
  // but are bound here too so a bare alias handle (no script instance) resolves.
  auto alias_ref = [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Object(Resolve(bindings).AliasReference(self));
  };
  reg.Register("ReferenceAlias", "GetReference", alias_ref);
  reg.Register("ReferenceAlias", "GetActorReference", alias_ref);
  reg.Register("ReferenceAlias", "GetActorRef", alias_ref);
  reg.Register("ReferenceAlias", "GetRef", alias_ref);
  // Runtime fill: a script can point an alias at a specific ref (ForceRefTo) or
  // empty it (Clear); the fill then wins in AliasReference until cleared.
  reg.Register("ReferenceAlias", "ForceRefTo", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).AliasForceRefTo(self, a.empty() ? ObjectRef{} : a[0].as_object());
    return Value();
  });
  reg.Register("ReferenceAlias", "Clear", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    Resolve(bindings).AliasClear(self);
    return Value();
  });
  // Lock(abLock=true,...): Lock(false) is the in-game way to unlock.
  reg.Register("ObjectReference", "Lock", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).SetLocked(self, ArgB(a, 0, true));
    return Value();
  });
  reg.Register("ObjectReference", "IsLocked", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(Resolve(bindings).IsLocked(self));
  });
  reg.Register("ObjectReference", "GetLockLevel", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Int(Resolve(bindings).GetLockLevel(self));
  });
  reg.Register("ObjectReference", "SetLockLevel", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).SetLockLevel(self, ArgI(a, 0));
    return Value();
  });
  reg.Register("ObjectReference", "GetOpenState", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Int(Resolve(bindings).GetOpenState(self));
  });
  reg.Register("ObjectReference", "SetOpen", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).SetOpen(self, ArgB(a, 0, true));
    return Value();
  });
}

void RegisterGlobalVariable(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  reg.Register("GlobalVariable", "GetValue", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Float(Resolve(bindings).GetGlobalValue(self));
  });
  reg.Register("GlobalVariable", "SetValue", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).SetGlobalValue(self, ArgF(a, 0));
    return Value();
  });
  reg.Register("GlobalVariable", "GetValueInt", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Int(static_cast<i32>(Resolve(bindings).GetGlobalValue(self)));
  });
  reg.Register("GlobalVariable", "SetValueInt", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).SetGlobalValue(self, static_cast<f32>(ArgI(a, 0)));
    return Value();
  });
  reg.Register("GlobalVariable", "Mod", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    SkyrimBindings& b = Resolve(bindings);
    b.SetGlobalValue(self, b.GetGlobalValue(self) + ArgF(a, 0));
    return Value();
  });
}

void RegisterCell(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  reg.Register("Cell", "IsInterior", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(Resolve(bindings).IsInterior(self));
  });
  reg.Register("Cell", "GetWaterLevel", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Float(Resolve(bindings).GetCellWaterLevel(self));
  });
}

void RegisterFaction(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  reg.Register("Faction", "GetReaction", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    return Value::Int(Resolve(bindings).GetReaction(self, ArgO(a, 0)));
  });
  reg.Register("Faction", "SetReaction", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).SetReaction(self, ArgO(a, 0), ArgI(a, 1));
    return Value();
  });
  reg.Register("Faction", "GetCrimeGold", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Int(Resolve(bindings).GetCrimeGold(self));
  });
  reg.Register("Faction", "SetCrimeGold", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).SetCrimeGold(self, ArgI(a, 0));
    return Value();
  });
  reg.Register("Faction", "ModCrimeGold", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).ModCrimeGold(self, ArgI(a, 0));
    return Value();
  });
}

void RegisterQuest(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  reg.Register("Quest", "GetStage", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Int(Resolve(bindings).GetStage(self));
  });
  reg.Register("Quest", "GetCurrentStageID", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Int(Resolve(bindings).GetStage(self));
  });
  // The script-level Quest.SetStage lowers to SetCurrentStageID, so this is the
  // native that actually advances the journal when a fragment calls SetStage().
  reg.Register("Quest", "SetCurrentStageID", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).SetStage(self, ArgI(a, 0));
    return Value::Bool(true);
  });
  reg.Register("Quest", "SetStage", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).SetStage(self, ArgI(a, 0));
    return Value::Bool(true);
  });
  reg.Register("Quest", "GetStageDone", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    return Value::Bool(Resolve(bindings).GetStageDone(self, ArgI(a, 0)));
  });
  // Scene fragments call Self.GetOwningQuest().SetStage(N) to advance the journal.
  reg.Register("Scene", "GetOwningQuest", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Object(Resolve(bindings).SceneOwningQuest(self));
  });
  // Quest stage fragments call MyScene.Start() to play a scene; the scene's own
  // fragments then SetStage, so this is the hinge of native scene-driven progress.
  reg.Register("Scene", "Start", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    Resolve(bindings).SceneStart(self);
    return Value();
  });
  reg.Register("Scene", "ForceStart", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    Resolve(bindings).SceneStart(self);
    return Value();
  });
  reg.Register("Scene", "Stop", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    Resolve(bindings).SceneStop(self);
    return Value();
  });
  reg.Register("Scene", "IsPlaying", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(Resolve(bindings).SceneIsPlaying(self));
  });
  reg.Register("Quest", "IsRunning", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(Resolve(bindings).IsRunning(self));
  });
  reg.Register("Quest", "Start", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    Resolve(bindings).StartQuest(self);
    return Value::Bool(true);
  });
  reg.Register("Quest", "Stop", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    Resolve(bindings).StopQuest(self);
    return Value();
  });
  reg.Register("Quest", "Reset", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    Resolve(bindings).ResetQuest(self);
    return Value();
  });
  reg.Register("Quest", "IsActive", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(Resolve(bindings).IsQuestActive(self));
  });
  reg.Register("Quest", "SetActive", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).SetQuestActive(self, ArgB(a, 0, true));
    return Value();
  });
  reg.Register("Quest", "SetObjectiveDisplayed", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).SetObjectiveDisplayed(self, ArgI(a, 0), ArgB(a, 1, true));
    return Value();
  });
  reg.Register("Quest", "SetObjectiveCompleted", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).SetObjectiveCompleted(self, ArgI(a, 0), ArgB(a, 1, true));
    return Value();
  });
  reg.Register("Quest", "IsObjectiveDisplayed", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    return Value::Bool(Resolve(bindings).IsObjectiveDisplayed(self, ArgI(a, 0)));
  });
  reg.Register("Quest", "IsObjectiveCompleted", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    return Value::Bool(Resolve(bindings).IsObjectiveCompleted(self, ArgI(a, 0)));
  });
}

void RegisterActor(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  reg.Register("Actor", "GetActorValue", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    return Value::Float(Resolve(bindings).GetActorValue(self, ArgS(a, 0)));
  });
  reg.Register("Actor", "GetBaseActorValue", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    return Value::Float(Resolve(bindings).GetBaseActorValue(self, ArgS(a, 0)));
  });
  reg.Register("Actor", "GetActorValuePercentage", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    return Value::Float(Resolve(bindings).GetActorValuePercentage(self, ArgS(a, 0)));
  });
  reg.Register("Actor", "SetActorValue", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).SetActorValue(self, ArgS(a, 0), ArgF(a, 1));
    return Value();
  });
  reg.Register("Actor", "ForceActorValue", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).ForceActorValue(self, ArgS(a, 0), ArgF(a, 1));
    return Value();
  });
  reg.Register("Actor", "ModActorValue", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).ModActorValue(self, ArgS(a, 0), ArgF(a, 1));
    return Value();
  });
  reg.Register("Actor", "DamageActorValue", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).ModActorValue(self, ArgS(a, 0), -ArgF(a, 1));
    return Value();
  });
  reg.Register("Actor", "RestoreActorValue", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).RestoreActorValue(self, ArgS(a, 0), ArgF(a, 1));
    return Value();
  });
  reg.Register("Actor", "GetLevel", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Int(Resolve(bindings).GetLevel(self));
  });
  reg.Register("Actor", "IsDead", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(Resolve(bindings).IsDead(self));
  });
  reg.Register("Actor", "IsInCombat", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(Resolve(bindings).IsInCombat(self));
  });
  reg.Register("Actor", "IsSneaking", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(Resolve(bindings).IsSneaking(self));
  });
  reg.Register("Actor", "GetCombatTarget", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Object(Resolve(bindings).GetCombatTarget(self));
  });
  reg.Register("Actor", "EquipItem", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).EquipItem(self, ArgO(a, 0));
    return Value();
  });
  reg.Register("Actor", "AddSpell", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).AddSpell(self, ArgO(a, 0));
    return Value();
  });
  reg.Register("Actor", "GetFactionRank", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    return Value::Int(Resolve(bindings).GetFactionRank(self, ArgO(a, 0)));
  });
  reg.Register("Actor", "SetFactionRank", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).SetFactionRank(self, ArgO(a, 0), ArgI(a, 1));
    return Value();
  });
  reg.Register("Actor", "IsInFaction", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    return Value::Bool(Resolve(bindings).IsInFaction(self, ArgO(a, 0)));
  });
  reg.Register("Actor", "AddToFaction", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).AddToFaction(self, ArgO(a, 0));
    return Value();
  });
  reg.Register("Actor", "RemoveFromFaction", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).RemoveFromFaction(self, ArgO(a, 0));
    return Value();
  });
}

}  // namespace

void RegisterSkyrimNatives(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  RegisterMath(reg);
  RegisterDebug(reg);
  RegisterUtility(reg, bindings);
  RegisterGameAndForms(reg, bindings);
  RegisterGameControls(reg, bindings);
  RegisterObjectReference(reg, bindings);
  RegisterActor(reg, bindings);
  RegisterQuest(reg, bindings);
  RegisterFaction(reg, bindings);
  RegisterCell(reg, bindings);
  RegisterGlobalVariable(reg, bindings);
}

}  // namespace rec::script::skyrim
