#include "script/games/skyrim/skyrim_natives_ext.h"

namespace rec::script::skyrim {

using papyrus::ObjectRef;
using papyrus::Value;
using papyrus::VirtualMachine;
using ext::Args;

void RegisterAliasExtra(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  // A bare alias handle does not carry its owning quest here, so report None.
  reg.Register("Alias", "GetOwningQuest", [](VirtualMachine&, ObjectRef, Args&) {
    return Value::Object(ObjectRef{});
  });

  auto noop = [](VirtualMachine&, ObjectRef, Args&) { return Value(); };
  reg.Register("Alias", "RegisterForAnimationEvent", noop);
  reg.Register("Alias", "RegisterForLOS", noop);
  reg.Register("Alias", "RegisterForSingleLOSGain", noop);
  reg.Register("Alias", "RegisterForSingleLOSLost", noop);
  reg.Register("Alias", "RegisterForSingleUpdateGameTime", noop);
  reg.Register("Alias", "RegisterForSleep", noop);
  reg.Register("Alias", "RegisterForTrackedStatsEvent", noop);
  reg.Register("Alias", "RegisterForUpdateGameTime", noop);
  reg.Register("Alias", "StartObjectProfiling", noop);
  reg.Register("Alias", "StopObjectProfiling", noop);
  reg.Register("Alias", "UnregisterForAnimationEvent", noop);
  reg.Register("Alias", "UnregisterForLOS", noop);
  reg.Register("Alias", "UnregisterForSleep", noop);
  reg.Register("Alias", "UnregisterForTrackedStatsEvent", noop);
  reg.Register("Alias", "UnregisterForUpdateGameTime", noop);

  reg.Register("LocationAlias", "Clear", noop);

  reg.Register("ReferenceAlias", "AddInventoryEventFilter", noop);
  reg.Register("ReferenceAlias", "RemoveAllInventoryEventFilters", noop);
  reg.Register("ReferenceAlias", "RemoveInventoryEventFilter", noop);
}

}  // namespace rec::script::skyrim
