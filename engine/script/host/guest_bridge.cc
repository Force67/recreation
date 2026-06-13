#include "script/host/guest_bridge.h"

#include <cstdint>
#include <string>

#include "script/papyrus_guest.h"

namespace rec::script::host {
namespace {

using papyrus::ObjectRef;
using papyrus::Value;
using papyrus::VirtualMachine;

PapyrusGuest& Guest(void* ctx) { return *static_cast<PapyrusGuest*>(ctx); }

std::int32_t LoadScript(void* ctx, const char* type) {
  std::string name = type;
  return Guest(ctx)
      .SubmitFor([name](VirtualMachine& vm) { return vm.HasScript(name) ? 1 : 0; })
      .get();
}

std::uint64_t CreateInstance(void* ctx, const char* type) {
  return Guest(ctx).CreateInstance(type).get().handle;
}

void RaiseEvent(void* ctx, std::uint64_t instance, const char* event) {
  Guest(ctx).RaiseEvent(ObjectRef{instance}, event);
}

std::int32_t GetIntProperty(void* ctx, std::uint64_t instance, const char* property) {
  std::string prop = property;
  return Guest(ctx)
      .SubmitFor([instance, prop](VirtualMachine& vm) {
        return vm.GetProperty(ObjectRef{instance}, prop).ToInt();
      })
      .get();
}

void SetIntProperty(void* ctx, std::uint64_t instance, const char* property, std::int32_t value) {
  std::string prop = property;
  Guest(ctx)
      .SubmitFor([instance, prop, value](VirtualMachine& vm) {
        vm.SetProperty(ObjectRef{instance}, prop, Value::Int(value));
        return 0;
      })
      .get();
}

void Tick(void* ctx, float dt) { Guest(ctx).Tick(dt); }

}  // namespace

GuestBridge MakeGuestBridge(PapyrusGuest& guest) {
  GuestBridge bridge{};
  bridge.ctx = &guest;
  bridge.load_script = &LoadScript;
  bridge.create_instance = &CreateInstance;
  bridge.raise_event = &RaiseEvent;
  bridge.get_int_property = &GetIntProperty;
  bridge.set_int_property = &SetIntProperty;
  bridge.tick = &Tick;
  return bridge;
}

}  // namespace rec::script::host
