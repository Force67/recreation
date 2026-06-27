#ifndef RECREATION_SCRIPT_PAPYRUS_VM_H_
#define RECREATION_SCRIPT_PAPYRUS_VM_H_

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/types.h"
#include "script/papyrus/interpreter.h"
#include "script/papyrus/native.h"
#include "script/papyrus/pex.h"
#include "script/papyrus/value.h"

namespace rec::script::papyrus {

// The Papyrus virtual machine: a registry of loaded script types, the live
// object instances, and the dispatch that ties them together (method/property
// resolution across the inheritance chain, states, parent calls, arrays). It
// implements VmInterface so the interpreter can call back into it for anything
// that leaves a single function frame.
//
// One VirtualMachine is the guest world for one game. It is not internally
// synchronized: the host runs it from a single dedicated thread (see the
// script runtime), keeping the guest cleanly compartmentalized.
class VirtualMachine : public VmInterface {
 public:
  // natives is the per-game function table (Skyrim, Fallout, ...). It must
  // outlive the VM. Pass nullptr for a VM with no engine bindings (tests).
  explicit VirtualMachine(const NativeRegistry* natives) : natives_(natives) {}

  // Parses and registers a compiled script. The script's object name becomes a
  // known type. Returns the type name, or "" on a parse failure.
  std::string LoadScript(ByteSpan pex_data);

  // Registers an already-parsed script. Returns the type name, or "" if the
  // file has no object.
  std::string AddScript(PexFile pex);

  bool HasScript(const std::string& type) const;
  size_t script_count() const { return scripts_.size(); }

  // A traced native invocation: the script type that declared it, the function
  // name, and a monotonic sequence number (1 = first since tracing began).
  struct NativeCall {
    std::string script_type;
    std::string function;
    u64 seq = 0;
  };
  // Native-call tracing for the debug overlay. Off by default (it copies two
  // strings per native call); the engine turns it on only while the trace
  // window is open. The ring keeps the most recent calls, newest last.
  void set_native_trace(bool on) { native_trace_enabled_ = on; }
  bool native_trace() const { return native_trace_enabled_; }
  u64 native_call_count() const { return native_call_count_; }
  const std::vector<NativeCall>& native_trace_log() const { return native_trace_; }
  void ClearNativeTrace() {
    native_trace_.clear();
    native_call_count_ = 0;
  }

  // Instantiates a known script type. Member variables are seeded from the
  // script's declared defaults. Returns None for an unknown type.
  ObjectRef CreateInstance(const std::string& type);
  // Same, but with a caller-chosen handle (the engine uses a form id, so an
  // object reference between scripts resolves to the same instance). Returns
  // None on unknown type or if the handle is taken.
  ObjectRef CreateInstanceWithHandle(const std::string& type, u64 handle);
  void DestroyInstance(ObjectRef instance);

  // The parent class name of a loaded type, "" if none or unknown. Used to
  // walk and lazily load the script's ancestor chain.
  std::string ParentClassOf(const std::string& type);
  bool IsAlive(ObjectRef instance) const;
  std::string TypeOf(ObjectRef instance);

  // Calls a method on an instance, or a global function on a script type. These
  // are the entry points the host and the event system use.
  Value Call(ObjectRef self, const std::string& method, std::vector<Value> args);
  Value CallGlobal(const std::string& script_type, const std::string& function,
                   std::vector<Value> args);

  // Like Call, but for optional event handlers: dispatches only when the
  // instance exists and actually defines the method, and never warns about a
  // missing one. Returns true if it dispatched. This lets the engine broadcast
  // events (OnDeath, OnItemAdded, ...) to forms whose script may not handle
  // them, without log spam.
  bool TryCall(ObjectRef self, const std::string& method, std::vector<Value> args);

  // VmInterface, used by the interpreter.
  Value CallMethod(ObjectRef self, const std::string& method, std::vector<Value> args) override;
  Value CallStatic(const std::string& script_type, const std::string& function,
                   std::vector<Value> args) override;
  Value CallParent(ObjectRef self, const std::string& method, std::vector<Value> args) override;
  Value GetProperty(ObjectRef self, const std::string& property) override;
  void SetProperty(ObjectRef self, const std::string& property, Value value) override;
  Value* MemberVar(ObjectRef self, const std::string& name) override;
  std::string CurrentState(ObjectRef self) override;
  void GotoState(ObjectRef self, const std::string& state) override;
  bool IsObjectOfType(ObjectRef obj, const std::string& type_name) override;
  ArrayRef ArrayCreate(const std::string& element_type, i32 size) override;
  i32 ArrayLength(ArrayRef array) override;
  Value ArrayGet(ArrayRef array, i32 index) override;
  void ArraySet(ArrayRef array, i32 index, Value value) override;
  i32 ArrayFind(ArrayRef array, const Value& value, i32 start) override;
  i32 ArrayRFind(ArrayRef array, const Value& value, i32 start) override;
  void ArrayAdd(ArrayRef array, const Value& value, i32 count) override;
  void ArrayInsert(ArrayRef array, i32 index, const Value& value) override;
  void ArrayRemove(ArrayRef array, i32 index, i32 count) override;
  void ArrayRemoveLast(ArrayRef array) override;
  void ArrayClear(ArrayRef array) override;
  StructRef StructCreate(const std::string& type_name) override;
  Value StructGet(StructRef instance, const std::string& member) override;
  void StructSet(StructRef instance, const std::string& member, Value value) override;

 private:
  struct LoadedScript {
    PexFile pex;
    std::string name;    // object (type) name, original case
    std::string parent;  // parent class name, "" if none
    const Object* object = nullptr;
  };
  struct Instance {
    // A form can carry several attached scripts (e.g. a quest's main script plus
    // its QF_ stage-fragment script). They share one handle and member store, in
    // attach order; types[0] is the primary (drives TypeOf and the default
    // state). Method/property resolution walks every attached script's chain.
    std::vector<std::string> types;
    std::unordered_map<std::string, Value> members;
    std::string state;  // "" = default/empty state
    const std::string& primary_type() const {
      static const std::string kEmpty;
      return types.empty() ? kEmpty : types.front();
    }
  };
  struct Resolved {
    LoadedScript* script = nullptr;
    const Function* fn = nullptr;
    std::string defining_type;  // script type that owns fn, for native lookup
  };

  LoadedScript* FindScript(const std::string& type);
  Instance* FindInstance(ObjectRef instance);

  // Resolves method on inst, starting the chain walk at start_type (the
  // instance type for normal calls, the parent type for parent calls).
  bool ResolveMethod(Instance& inst, const std::string& method, const std::string& start_type,
                     Resolved* out);
  // Resolves method across every script attached to inst (each script's own
  // inheritance chain, in attach order); the first match wins. This is how a
  // stage fragment finds its function regardless of which attached script the
  // function lives on.
  bool ResolveMethodAny(Instance& inst, const std::string& method, Resolved* out);
  const Property* ResolveProperty(Instance& inst, const std::string& name,
                                  LoadedScript** owner_script);
  Value Invoke(const Resolved& target, ObjectRef self, std::vector<Value> args,
               const std::string& method_name);
  void SeedMembers(Instance& inst, const std::string& type);
  bool ArrayValid(ArrayRef array) const { return array.id != 0 && array.id <= arrays_.size(); }
  bool StructValid(StructRef s) const { return s.id != 0 && s.id <= structs_.size(); }
  void WarnUnbound(const std::string& type, const std::string& function);
  // Appends a native invocation to the trace ring when tracing is enabled.
  void RecordNative(const std::string& type, const std::string& function);

  static constexpr size_t kNativeTraceCap = 256;
  bool native_trace_enabled_ = false;
  u64 native_call_count_ = 0;
  std::vector<NativeCall> native_trace_;  // ring, oldest at front

  const NativeRegistry* natives_;
  std::unordered_map<std::string, LoadedScript> scripts_;  // key: lowercased type
  std::unordered_map<u64, Instance> instances_;
  u64 next_handle_ = 1;
  std::vector<std::vector<Value>> arrays_;  // 1-based; index 0 reserved as None
  std::vector<std::unordered_map<std::string, Value>> structs_;  // 1-based; Fallout structs
  std::vector<std::string> call_stack_;     // executing script type, for parent calls
  std::unordered_set<std::string> warned_;  // unbound natives warned once
};

}  // namespace rec::script::papyrus

#endif  // RECREATION_SCRIPT_PAPYRUS_VM_H_
