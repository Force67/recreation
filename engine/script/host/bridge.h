#ifndef RECREATION_SCRIPT_HOST_BRIDGE_H_
#define RECREATION_SCRIPT_HOST_BRIDGE_H_

#include <cstdint>

namespace rec::script::host {

// The C ABI the .NET host (the user-facing "main" world where mods live) calls
// to reach the engine through its Papyrus guest. The managed side mirrors these
// layouts exactly (Recreation.Scripting, Interop/*). The two worlds share
// nothing but this table of function pointers and an opaque context, which keeps
// them cleanly separated: managed code never sees a VM type, the VM never sees a
// CLR type.
//
// Design: a value-based dispatch surface. Rather than one C function per game
// API, the bridge marshals a dynamically typed value (ApiValue) and routes
// calls through the VM's CallGlobal/CallMethod. Those already fall back to the
// engine's native registry, so a single pair of dispatch functions exposes the
// entire game API (Game.GetPlayer, Actor.GetActorValue, ObjectReference.MoveTo,
// ...) plus any loaded Papyrus script function. New game functionality needs no
// new bridge entry: it is reachable the moment the engine registers the native.
//
// Stability: only ever append fields to ScriptBridge and values to ApiKind.
// Reordering or inserting breaks managed builds compiled against an older
// layout. All strings are UTF-8, null terminated. Object handles are opaque
// (0 means none/failure).

// The kind tag of an ApiValue. Mirrors papyrus::ValueType for the kinds that
// cross the boundary; struct instances are not marshalled (kNone instead).
enum class ApiKind : std::int32_t {
  kNone = 0,
  kInt = 1,
  kFloat = 2,
  kBool = 3,
  kString = 4,
  kObject = 5,  // an object/form handle, in `h`
  kArray = 6,   // a VM array id, in `h` (opaque to managed code)
};

// A dynamically typed value crossing the boundary. POD with a fixed layout the
// managed side mirrors byte for byte. Only one payload field is meaningful per
// kind. For kString results, `s` points at engine-owned storage valid until the
// next bridge call on the same thread; managed code copies it out immediately.
struct ApiValue {
  ApiKind kind = ApiKind::kNone;
  std::int32_t i = 0;     // kInt, or kBool (0/1)
  float f = 0.0f;         // kFloat
  std::uint64_t h = 0;    // kObject handle / kArray id
  const char* s = nullptr;  // kString (borrowed UTF-8)
};

// The function table. ctx is the engine-side guest wrapper, opaque to managed
// code and passed back into every call.
struct ScriptBridge {
  void* ctx;

  // --- Script and instance management ---------------------------------------
  // Returns 1 if a script type of this name is loaded in the guest, else 0.
  std::int32_t (*is_script_loaded)(void* ctx, const char* type_name);
  // Loads type_name (and its ancestor chain) from the asset VFS if not already
  // present. Returns 1 on success, 0 if the .pex is unavailable.
  std::int32_t (*load_script)(void* ctx, const char* type_name);
  // Creates an instance of a loaded script type. Returns its handle, 0 on fail.
  std::uint64_t (*create_instance)(void* ctx, const char* type_name);
  // Writes the script type name of a handle into buf (UTF-8, truncated to
  // buf_len) and returns the full length, or 0 if the handle is not an instance.
  std::int32_t (*type_of)(void* ctx, std::uint64_t handle, char* buf, std::int32_t buf_len);

  // --- Dispatch (the heart of moddability) ----------------------------------
  // Calls a global function on a script type (e.g. "Game", "GetPlayer"), or a
  // method on an instance/form handle. args points at argc ApiValues. The result
  // is written through result. Both route through the VM, which falls back to the
  // engine native registry, so any registered native or loaded script function
  // is reachable.
  void (*call_global)(void* ctx, const char* type_name, const char* function,
                      const ApiValue* args, std::int32_t argc, ApiValue* result);
  void (*call_method)(void* ctx, std::uint64_t self, const char* function, const ApiValue* args,
                      std::int32_t argc, ApiValue* result);

  // --- Properties -----------------------------------------------------------
  void (*get_property)(void* ctx, std::uint64_t self, const char* name, ApiValue* result);
  void (*set_property)(void* ctx, std::uint64_t self, const char* name, ApiValue value);

  // --- Time -----------------------------------------------------------------
  // Advances the guest clock, firing any due update events.
  void (*tick)(void* ctx, float dt);
};

// An engine event delivered into the managed event bus, so mods react to the
// world (gmod-style hooks). POD with a fixed layout the managed side mirrors.
// Only the payload fields an id documents are meaningful. Append only.
enum class ManagedEventId : std::int32_t {
  kNone = 0,
  kActorDied = 1,           // a = actor handle
  kItemAdded = 2,           // a = container handle, b = item handle, i = count
  kQuestStageChanged = 3,   // a = quest form id, i = stage
  kFormLoaded = 4,          // a = form handle (its scripts just attached / it went live)
};

struct ManagedEvent {
  ManagedEventId id = ManagedEventId::kNone;
  std::uint64_t a = 0;
  std::uint64_t b = 0;
  std::int32_t i = 0;
  float f = 0.0f;
};

// The outbound half of the boundary: calls from the engine into the managed
// world. The managed entrypoint fills these during boot, and the engine then
// drives the managed host through them: tick once per frame, publish_event when
// the world raises an event, shutdown on teardown. Any pointer may be null if
// the managed side declines that hook.
struct HostCallbacks {
  void (*tick)(float dt);
  void (*publish_event)(const ManagedEvent* e);
  void (*shutdown)();
};

// What the managed entrypoint receives: the inbound bridge it drives the engine
// through, plus the outbound callbacks slot it fills. One struct keeps the
// entrypoint a single-argument call.
struct HostHandshake {
  ScriptBridge* bridge;
  HostCallbacks callbacks;
};

// Signature of the managed entrypoint, exported [UnmanagedCallersOnly]. The host
// resolves a function pointer of this type and calls it with the handshake.
using ManagedEntry = std::int32_t (*)(HostHandshake*);

}  // namespace rec::script::host

#endif  // RECREATION_SCRIPT_HOST_BRIDGE_H_
