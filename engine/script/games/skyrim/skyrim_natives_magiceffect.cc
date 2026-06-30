#include "script/games/skyrim/skyrim_natives_ext.h"

namespace rec::script::skyrim {

using papyrus::ObjectRef;
using papyrus::Value;
using papyrus::VirtualMachine;
using ext::Args;

void RegisterActiveMagicEffectExtra(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  // An active magic effect is a transient runtime instance the engine does not
  // model yet, so its base form, caster, and target resolve to None.
  auto none = [](VirtualMachine&, ObjectRef, Args&) { return Value::Object(ObjectRef{}); };
  reg.Register("ActiveMagicEffect", "GetBaseObject", none);
  reg.Register("ActiveMagicEffect", "GetCasterActor", none);
  reg.Register("ActiveMagicEffect", "GetTargetActor", none);

  auto noop = [](VirtualMachine&, ObjectRef, Args&) { return Value(); };
  reg.Register("ActiveMagicEffect", "AddInventoryEventFilter", noop);
  reg.Register("ActiveMagicEffect", "Dispel", noop);
  reg.Register("ActiveMagicEffect", "RegisterForAnimationEvent", noop);
  reg.Register("ActiveMagicEffect", "RegisterForLOS", noop);
  reg.Register("ActiveMagicEffect", "RegisterForSingleLOSGain", noop);
  reg.Register("ActiveMagicEffect", "RegisterForSingleLOSLost", noop);
  reg.Register("ActiveMagicEffect", "RegisterForSingleUpdateGameTime", noop);
  reg.Register("ActiveMagicEffect", "RegisterForSleep", noop);
  reg.Register("ActiveMagicEffect", "RegisterForTrackedStatsEvent", noop);
  reg.Register("ActiveMagicEffect", "RegisterForUpdateGameTime", noop);
  reg.Register("ActiveMagicEffect", "RemoveAllInventoryEventFilters", noop);
  reg.Register("ActiveMagicEffect", "RemoveInventoryEventFilter", noop);
  reg.Register("ActiveMagicEffect", "StartObjectProfiling", noop);
  reg.Register("ActiveMagicEffect", "StopObjectProfiling", noop);
  reg.Register("ActiveMagicEffect", "UnregisterForAnimationEvent", noop);
  reg.Register("ActiveMagicEffect", "UnregisterForLOS", noop);
  reg.Register("ActiveMagicEffect", "UnregisterForSleep", noop);
  reg.Register("ActiveMagicEffect", "UnregisterForTrackedStatsEvent", noop);
  reg.Register("ActiveMagicEffect", "UnregisterForUpdateGameTime", noop);
}

}  // namespace rec::script::skyrim
