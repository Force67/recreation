#include "script/games/skyrim/skyrim_native_state.h"
#include "script/games/skyrim/skyrim_natives_ext.h"

namespace rx::script::skyrim {

using papyrus::ObjectRef;
using papyrus::Value;
using papyrus::VirtualMachine;
using ext::Args;
using ext::ArgB; using ext::ArgF; using ext::ArgI; using ext::ArgS;
namespace st = state;

void RegisterUtilityRest(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  reg.Register("Utility", "SetINIBool", [](VirtualMachine&, ObjectRef, Args& a) {
    st::SetFlag(ObjectRef{1}, ArgS(a, 0), ArgB(a, 1, false));
    return Value();
  });
  reg.Register("Utility", "SetINIInt", [](VirtualMachine&, ObjectRef, Args& a) {
    st::SetInt(ObjectRef{1}, ArgS(a, 0), ArgI(a, 1));
    return Value();
  });
  reg.Register("Utility", "SetINIFloat", [](VirtualMachine&, ObjectRef, Args& a) {
    st::SetFloat(ObjectRef{1}, ArgS(a, 0), ArgF(a, 1));
    return Value();
  });
  reg.Register("Utility", "SetINIString", [](VirtualMachine&, ObjectRef, Args&) {
    // No string store exists, so the value is dropped.
    return Value();
  });

  reg.Register("Utility", "CaptureFrameRate", [](VirtualMachine&, ObjectRef, Args&) {
    return Value();
  });
  reg.Register("Utility", "StartFrameRateCapture", [](VirtualMachine&, ObjectRef, Args&) {
    return Value();
  });
  reg.Register("Utility", "EndFrameRateCapture", [](VirtualMachine&, ObjectRef, Args&) {
    return Value();
  });
  reg.Register("Utility", "GetBudgetCount", [](VirtualMachine&, ObjectRef, Args&) {
    return Value::Int(0);
  });
  reg.Register("Utility", "GetBudgetName", [](VirtualMachine&, ObjectRef, Args&) {
    return Value::Str("");
  });
  reg.Register("Utility", "GetCurrentBudget", [](VirtualMachine&, ObjectRef, Args&) {
    return Value::Float(0.0f);
  });
  reg.Register("Utility", "GetCurrentMemory", [](VirtualMachine&, ObjectRef, Args&) {
    return Value::Int(0);
  });
  reg.Register("Utility", "OverBudget", [](VirtualMachine&, ObjectRef, Args&) {
    return Value::Bool(false);
  });
}

}  // namespace rx::script::skyrim
