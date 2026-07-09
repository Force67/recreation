#include "script/games/skyrim/skyrim_native_state.h"
#include "script/games/skyrim/skyrim_natives_ext.h"

namespace rx::script::skyrim {

using papyrus::ObjectRef;
using papyrus::Value;
using papyrus::VirtualMachine;
using ext::Args;
using ext::ArgB; using ext::ArgI; using ext::ArgO;
using ext::Resolve;
namespace st = state;

void RegisterFactionExtra(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  // Crime gold splits into a violent and a non-violent share. The binding tracks
  // the total (GetCrimeGold); the violent share lives in the shared state on the
  // faction, and the non-violent share is the remainder.
  reg.Register("Faction", "SetCrimeGoldViolent", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetInt(self, "crimeGoldViolent", ArgI(a, 0));
    return Value();
  });
  reg.Register("Faction", "GetCrimeGoldViolent", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Int(st::GetInt(self, "crimeGoldViolent"));
  });
  reg.Register("Faction", "GetCrimeGoldNonViolent", [bindings](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Int(Resolve(bindings).GetCrimeGold(self) - st::GetInt(self, "crimeGoldViolent"));
  });

  // Infamy and stolen-item bookkeeping have no engine source yet.
  auto zero = [](VirtualMachine&, ObjectRef, Args&) { return Value::Int(0); };
  reg.Register("Faction", "GetInfamy", zero);
  reg.Register("Faction", "GetInfamyNonViolent", zero);
  reg.Register("Faction", "GetInfamyViolent", zero);
  reg.Register("Faction", "GetStolenItemValueCrime", zero);
  reg.Register("Faction", "GetStolenItemValueNoCrime", zero);

  reg.Register("Faction", "CanPayCrimeGold",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(true); });
  reg.Register("Faction", "IsFactionInCrimeGroup",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });

  // Expulsion and player-enemy status round-trip through the shared state.
  reg.Register("Faction", "IsPlayerExpelled", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(st::GetFlag(self, "playerExpelled"));
  });
  reg.Register("Faction", "SetPlayerExpelled", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetFlag(self, "playerExpelled", ArgB(a, 0, true));
    return Value();
  });
  reg.Register("Faction", "SetPlayerEnemy", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetFlag(self, "playerEnemy", ArgB(a, 0, true));
    return Value();
  });

  // Reaction edits route through the existing reaction binding.
  reg.Register("Faction", "ModReaction", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    auto& b = Resolve(bindings);
    b.SetReaction(self, ArgO(a, 0), b.GetReaction(self, ArgO(a, 0)) + ArgI(a, 1));
    return Value();
  });
  reg.Register("Faction", "SetAlly", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).SetReaction(self, ArgO(a, 0), +1);
    return Value();
  });
  reg.Register("Faction", "SetEnemy", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    Resolve(bindings).SetReaction(self, ArgO(a, 0), -1);
    return Value();
  });

  // These drive crime and jail systems the engine does not have yet.
  auto noop = [](VirtualMachine&, ObjectRef, Args&) { return Value(); };
  reg.Register("Faction", "PlayerPayCrimeGold", noop);
  reg.Register("Faction", "SendPlayerToJail", noop);
  reg.Register("Faction", "SendAssaultAlarm", noop);
}

}  // namespace rx::script::skyrim
