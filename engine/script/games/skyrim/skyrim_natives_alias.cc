#include "script/games/skyrim/skyrim_natives_ext.h"
#include "script/papyrus/alias_handle.h"

namespace rx::script::skyrim {

using papyrus::ObjectRef;
using papyrus::Value;
using papyrus::VirtualMachine;
using ext::Args;

void RegisterAliasExtra(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  // An alias handle packs its owning quest (see EncodeAliasHandle); unpack it.
  reg.Register("Alias", "GetOwningQuest", [](VirtualMachine&, ObjectRef self, Args&) {
    if (!papyrus::IsAliasHandle(self.handle)) return Value::Object(ObjectRef{});
    return Value::Object(ObjectRef{papyrus::AliasHandleQuest(self.handle)});
  });

  auto noop = [](VirtualMachine&, ObjectRef, Args&) { return Value(); };
  reg.Register("Alias", "RegisterForAnimationEvent", noop);
  reg.Register("Alias", "RegisterForSleep", noop);
  reg.Register("Alias", "RegisterForTrackedStatsEvent", noop);
  reg.Register("Alias", "StartObjectProfiling", noop);
  reg.Register("Alias", "StopObjectProfiling", noop);
  reg.Register("Alias", "UnregisterForAnimationEvent", noop);
  reg.Register("Alias", "UnregisterForSleep", noop);
  reg.Register("Alias", "UnregisterForTrackedStatsEvent", noop);

  reg.Register("LocationAlias", "Clear", noop);

  reg.Register("ReferenceAlias", "AddInventoryEventFilter", noop);
  reg.Register("ReferenceAlias", "RemoveAllInventoryEventFilters", noop);
  reg.Register("ReferenceAlias", "RemoveInventoryEventFilter", noop);
}

}  // namespace rx::script::skyrim
