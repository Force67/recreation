#ifndef RECREATION_SCRIPT_HOST_BRIDGE_H_
#define RECREATION_SCRIPT_HOST_BRIDGE_H_

#include <cstdint>

namespace rec::script::host {

// The C ABI the .NET host (the user-facing "main" world) calls to reach the
// Papyrus guest. The managed side mirrors this exact layout (Recreation.Scripting
// ScriptHost.cs). The two worlds share nothing but this table of function
// pointers and an opaque context, which keeps them cleanly separated: managed
// code never sees a VM type, the VM never sees a CLR type.
//
// Stability: only ever append fields. Reordering or inserting breaks managed
// builds compiled against an older layout. All strings are UTF-8, null
// terminated. Instance handles are opaque (0 means none/failure).
struct GuestBridge {
  void* ctx;  // the engine-side guest wrapper; opaque to managed code

  // Returns 1 if a script type of this name is loaded in the guest, else 0.
  std::int32_t (*load_script)(void* ctx, const char* type_name);
  // Creates an instance of a loaded script type. Returns its handle, 0 on fail.
  std::uint64_t (*create_instance)(void* ctx, const char* type_name);
  // Queues a no-argument event (method) call on an instance.
  void (*raise_event)(void* ctx, std::uint64_t instance, const char* event_name);
  // Reads/writes an Int property on an instance (synchronous).
  std::int32_t (*get_int_property)(void* ctx, std::uint64_t instance, const char* property);
  void (*set_int_property)(void* ctx, std::uint64_t instance, const char* property,
                           std::int32_t value);
  // Advances the guest clock, firing any due update events.
  void (*tick)(void* ctx, float dt);
};

// Signature of the managed entrypoint, exported [UnmanagedCallersOnly]. The
// host resolves a function pointer of this type and calls it with the bridge.
using ManagedEntry = std::int32_t (*)(GuestBridge*);

}  // namespace rec::script::host

#endif  // RECREATION_SCRIPT_HOST_BRIDGE_H_
