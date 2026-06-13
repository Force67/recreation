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
  // Player controls default to enabled, so the neutral "false" would be wrong;
  // register them explicitly. A control system can override later.
  static const char* const kControls[] = {
      "IsActivateControlsEnabled",  "IsCamSwitchControlsEnabled", "IsFastTravelControlsEnabled",
      "IsFightingControlsEnabled",  "IsJournalControlsEnabled",   "IsLookingControlsEnabled",
      "IsMenuControlsEnabled",      "IsMovementControlsEnabled",  "IsSneakingControlsEnabled",
      "IsFastTravelEnabled"};
  for (const char* name : kControls)
    reg.Register("Game", name, [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(true); });
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
}

void RegisterActor(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  reg.Register("Actor", "GetActorValue", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    return Value::Float(Resolve(bindings).GetActorValue(self, ArgS(a, 0)));
  });
  reg.Register("Actor", "SetActorValue", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).SetActorValue(self, ArgS(a, 0), ArgF(a, 1));
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
    Resolve(bindings).ModActorValue(self, ArgS(a, 0), ArgF(a, 1));
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
}

}  // namespace rec::script::skyrim
