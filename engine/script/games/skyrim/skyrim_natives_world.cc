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

void RegisterWorldExtra(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  // Quest: journal-completion and objective-failure state the campaign scripts
  // poll. The stage/objective primitives live in RegisterQuest; these layer the
  // remaining queries on top, backed by the shared state store.
  reg.Register("Quest", "CompleteQuest", [](VirtualMachine&, ObjectRef self, Args&) {
    st::SetFlag(self, "completed", true);
    return Value();
  });
  reg.Register("Quest", "IsCompleted", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(st::GetFlag(self, "completed"));
  });
  reg.Register("Quest", "CompleteAllObjectives",
               [](VirtualMachine&, ObjectRef, Args&) { return Value(); });
  reg.Register("Quest", "FailAllObjectives",
               [](VirtualMachine&, ObjectRef, Args&) { return Value(); });
  reg.Register("Quest", "IsObjectiveFailed", [](VirtualMachine&, ObjectRef self, Args& a) {
    return Value::Bool(st::GetFlag(self, "objFailed_" + std::to_string(ArgI(a, 0))));
  });
  // IsStageDone is the Quest-side mirror of GetStageDone (RegisterQuest), so route
  // it to the same binding.
  reg.Register("Quest", "IsStageDone", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    return Value::Bool(Resolve(bindings).GetStageDone(self, ArgI(a, 0)));
  });
  // We do not model the transient start/stop edge, so a quest is never observed
  // mid-transition; aliases are not resolvable through the Quest object yet.
  reg.Register("Quest", "IsStarting",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Quest", "IsStopping",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Quest", "GetAlias",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Object(ObjectRef{}); });
  reg.Register("Quest", "UpdateCurrentInstanceGlobal",
               [](VirtualMachine&, ObjectRef, Args&) { return Value(); });

  // Cell: ownership and public/attached state round-trip through the store; fog
  // overrides are accepted but have no renderer hook yet. Reset wipes the cell's
  // stored state.
  reg.Register("Cell", "SetActorOwner", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetRef(self, "actorOwner", ArgO(a, 0));
    return Value();
  });
  reg.Register("Cell", "GetActorOwner", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Object(st::GetRef(self, "actorOwner"));
  });
  reg.Register("Cell", "SetFactionOwner", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetRef(self, "factionOwner", ArgO(a, 0));
    return Value();
  });
  reg.Register("Cell", "GetFactionOwner", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Object(st::GetRef(self, "factionOwner"));
  });
  reg.Register("Cell", "SetPublic", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetFlag(self, "public", ArgB(a, 0, true));
    return Value();
  });
  reg.Register("Cell", "IsAttached",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Cell", "Reset", [](VirtualMachine&, ObjectRef self, Args&) {
    st::Clear(self);
    return Value();
  });
  auto cell_noop = [](VirtualMachine&, ObjectRef, Args&) { return Value(); };
  reg.Register("Cell", "SetFogColor", cell_noop);
  reg.Register("Cell", "SetFogPlanes", cell_noop);
  reg.Register("Cell", "SetFogPower", cell_noop);

  // Location: cleared state round-trips; the ref-type population and parent/child
  // queries have no location graph behind them yet, so they report the empty
  // defaults (no matching refs, no relationship, not loaded).
  reg.Register("Location", "SetCleared", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetFlag(self, "cleared", ArgB(a, 0, true));
    return Value();
  });
  reg.Register("Location", "IsCleared", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(st::GetFlag(self, "cleared"));
  });
  reg.Register("Location", "GetRefTypeAliveCount",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Int(0); });
  reg.Register("Location", "GetRefTypeDeadCount",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Int(0); });
  reg.Register("Location", "HasCommonParent",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Location", "HasRefType",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Location", "IsChild",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });
  reg.Register("Location", "IsLoaded",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Bool(false); });

  // ActorBase: the protection toggles round-trip through the store (IsEssential
  // routes to the binding in RegisterGameAndForms, so it is not re-bound here).
  // Class/gift-filter records and the dead counter have no data behind them yet.
  reg.Register("ActorBase", "SetEssential", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetFlag(self, "essential", ArgB(a, 0, true));
    return Value();
  });
  reg.Register("ActorBase", "SetProtected", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetFlag(self, "protected", ArgB(a, 0, true));
    return Value();
  });
  reg.Register("ActorBase", "IsProtected", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(st::GetFlag(self, "protected"));
  });
  reg.Register("ActorBase", "SetInvulnerable", [](VirtualMachine&, ObjectRef self, Args& a) {
    st::SetFlag(self, "invulnerable", ArgB(a, 0, true));
    return Value();
  });
  reg.Register("ActorBase", "IsInvulnerable", [](VirtualMachine&, ObjectRef self, Args&) {
    return Value::Bool(st::GetFlag(self, "invulnerable"));
  });
  reg.Register("ActorBase", "GetClass",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Object(ObjectRef{}); });
  reg.Register("ActorBase", "GetGiftFilter",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Object(ObjectRef{}); });
  reg.Register("ActorBase", "GetDeadCount",
               [](VirtualMachine&, ObjectRef, Args&) { return Value::Int(0); });
  reg.Register("ActorBase", "SetOutfit",
               [](VirtualMachine&, ObjectRef, Args&) { return Value(); });
}

}  // namespace rec::script::skyrim
