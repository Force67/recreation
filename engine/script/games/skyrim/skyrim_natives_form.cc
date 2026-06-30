#include "script/games/skyrim/skyrim_natives_ext.h"

namespace rec::script::skyrim {

using papyrus::ObjectRef;
using papyrus::Value;
using papyrus::VirtualMachine;
using ext::Args;

void RegisterFormExtra(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  // No player knowledge model yet, so nothing is known.
  reg.Register("Form", "PlayerKnows", [](VirtualMachine&, ObjectRef, Args&) {
    return Value::Bool(false);
  });

  auto noop = [](VirtualMachine&, ObjectRef, Args&) { return Value(); };
  reg.Register("Form", "RegisterForAnimationEvent", noop);
  reg.Register("Form", "RegisterForLOS", noop);
  reg.Register("Form", "RegisterForSingleLOSGain", noop);
  reg.Register("Form", "RegisterForSingleLOSLost", noop);
  reg.Register("Form", "RegisterForSingleUpdateGameTime", noop);
  reg.Register("Form", "RegisterForSleep", noop);
  reg.Register("Form", "RegisterForTrackedStatsEvent", noop);
  reg.Register("Form", "RegisterForUpdateGameTime", noop);
  reg.Register("Form", "StartObjectProfiling", noop);
  reg.Register("Form", "StopObjectProfiling", noop);
  reg.Register("Form", "UnregisterForAnimationEvent", noop);
  reg.Register("Form", "UnregisterForLOS", noop);
  reg.Register("Form", "UnregisterForSleep", noop);
  reg.Register("Form", "UnregisterForTrackedStatsEvent", noop);
  reg.Register("Form", "UnregisterForUpdateGameTime", noop);
}

}  // namespace rec::script::skyrim
