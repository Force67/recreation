#include "script/papyrus/vm.h"

#include "script/papyrus/fiber.h"

#include <algorithm>
#include <cctype>

#include "core/log.h"

namespace rx::script::papyrus {
namespace {

std::string Lower(std::string s) {
  std::ranges::transform(s, s.begin(),
                         [](char c) { return static_cast<char>(std::tolower((unsigned char)c)); });
  return s;
}

Value ValueFromData(const PexFile& pex, const VariableData& v) {
  switch (v.type) {
    case VariableData::Type::kInteger:
      return Value::Int(v.int_value);
    case VariableData::Type::kFloat:
      return Value::Float(v.float_value);
    case VariableData::Type::kBool:
      return Value::Bool(v.bool_value);
    case VariableData::Type::kString:
      return Value::Str(pex.Str(v.string_index));
    default:
      return Value();
  }
}

Value DefaultForTypeName(const std::string& type_name) {
  std::string t = Lower(type_name);
  if (t == "int") return Value::Int(0);
  if (t == "float") return Value::Float(0);
  if (t == "bool") return Value::Bool(false);
  if (t == "string") return Value::Str("");
  return Value();
}

// Finds method within one script's named state. An empty state_name selects the
// default/empty state. Returns null when the state exists but lacks the method,
// or the state is not declared by this script.
const Function* FindInState(const PexFile& pex, const Object& object, const std::string& state_name,
                            const std::string& method) {
  std::string want_state = Lower(state_name);
  std::string want_fn = Lower(method);
  for (const State& st : object.states) {
    if (Lower(pex.Str(st.name)) != want_state) continue;
    for (const NamedFunction& nf : st.functions)
      if (Lower(pex.Str(nf.name)) == want_fn) return &nf.function;
    return nullptr;  // state matched but no such function
  }
  return nullptr;
}

}  // namespace

std::string VirtualMachine::LoadScript(ByteSpan pex_data) {
  PexFile pex;
  if (!ParsePex(pex_data, &pex)) return "";
  return AddScript(std::move(pex));
}

std::string VirtualMachine::AddScript(PexFile pex) {
  if (pex.objects.empty()) return "";
  LoadedScript ls;
  ls.pex = std::move(pex);
  ls.name = ls.pex.Str(ls.pex.objects[0].name);
  ls.parent = ls.pex.Str(ls.pex.objects[0].parent_class);
  std::string key = Lower(ls.name);
  LoadedScript& stored = (scripts_[key] = std::move(ls));
  stored.object = &stored.pex.objects[0];  // fix pointer after the move
  return stored.name;
}

bool VirtualMachine::HasScript(const std::string& type) const {
  return scripts_.count(Lower(type)) != 0;
}

VirtualMachine::LoadedScript* VirtualMachine::FindScript(const std::string& type) {
  auto it = scripts_.find(Lower(type));
  return it == scripts_.end() ? nullptr : &it->second;
}

VirtualMachine::Instance* VirtualMachine::FindInstance(ObjectRef instance) {
  auto it = instances_.find(instance.handle);
  return it == instances_.end() ? nullptr : &it->second;
}

void VirtualMachine::SeedMembers(Instance& inst, const std::string& type) {
  for (LoadedScript* s = FindScript(type); s; s = FindScript(s->parent)) {
    for (const MemberVariable& v : s->object->variables) {
      std::string name = s->pex.Str(v.name);
      if (inst.members.count(name)) continue;
      inst.members[name] = ValueFromData(s->pex, v.initial_value);
    }
  }
}

ObjectRef VirtualMachine::CreateInstance(const std::string& type) {
  return CreateInstanceWithHandle(type, next_handle_++);
}

ObjectRef VirtualMachine::CreateInstanceWithHandle(const std::string& type, u64 handle) {
  LoadedScript* s = FindScript(type);
  if (!s || handle == 0) return ObjectRef{0};
  // A handle may already carry other attached scripts. Add this one to the same
  // instance rather than failing, so every script on a form (quest main script
  // AND its QF_ stage-fragment script, etc.) is live and dispatchable. Attaching
  // the same script twice is a no-op: return 0 so the caller skips re-seeding and
  // re-raising OnInit.
  Instance& inst = instances_[handle];
  for (const std::string& t : inst.types)
    if (Lower(t) == Lower(s->name)) return ObjectRef{0};
  if (handle >= next_handle_) next_handle_ = handle + 1;
  inst.types.push_back(s->name);
  SeedMembers(inst, s->name);
  return ObjectRef{handle};
}

bool VirtualMachine::HasAttachedScript(ObjectRef instance, const std::string& type) const {
  auto it = instances_.find(instance.handle);
  if (it == instances_.end()) return false;
  const std::string want = Lower(type);
  for (const std::string& attached : it->second.types)
    if (Lower(attached) == want) return true;
  return false;
}

std::string VirtualMachine::ParentClassOf(const std::string& type) {
  LoadedScript* s = FindScript(type);
  return s ? s->parent : "";
}

void VirtualMachine::DestroyInstance(ObjectRef instance) { instances_.erase(instance.handle); }

bool VirtualMachine::IsAlive(ObjectRef instance) const {
  return instance.handle != 0 && instances_.count(instance.handle) != 0;
}

std::string VirtualMachine::TypeOf(ObjectRef instance) {
  Instance* inst = FindInstance(instance);
  return inst ? inst->primary_type() : "";
}

bool VirtualMachine::ResolveMethodAny(Instance& inst, const std::string& method, Resolved* out) {
  for (const std::string& type : inst.types)
    if (ResolveMethod(inst, method, type, out)) return true;
  return false;
}

bool VirtualMachine::ResolveMethod(Instance& inst, const std::string& method,
                                   const std::string& start_type, Resolved* out) {
  for (LoadedScript* s = FindScript(start_type); s; s = FindScript(s->parent)) {
    if (!inst.state.empty()) {
      if (const Function* f = FindInState(s->pex, *s->object, inst.state, method)) {
        *out = {s, f, s->name};
        return true;
      }
    }
    if (const Function* f = FindInState(s->pex, *s->object, "", method)) {
      *out = {s, f, s->name};
      return true;
    }
  }
  return false;
}

const Property* VirtualMachine::ResolveProperty(Instance& inst, const std::string& name,
                                                LoadedScript** owner_script) {
  std::string want = Lower(name);
  for (const std::string& type : inst.types)
    for (LoadedScript* s = FindScript(type); s; s = FindScript(s->parent))
      for (const Property& p : s->object->properties)
        if (Lower(s->pex.Str(p.name)) == want) {
          *owner_script = s;
          return &p;
        }
  return nullptr;
}

Value VirtualMachine::Invoke(const Resolved& target, ObjectRef self, std::vector<Value> args,
                             const std::string& method_name) {
  if (target.fn->is_native) {
    if (natives_) {
      // Walk the instance's script chain (most-derived first) so a native bound on
      // a base class resolves for a derived script even when the method happens to
      // be *declared* on a different ancestor. A CWReinforcementAliasScript
      // (extends ReferenceAlias extends Alias) thus reaches the ReferenceAlias.*
      // engine bindings; without the walk the lookup only tried the declaring type
      // and silently returned None for the whole per-actor Civil War layer.
      if (Instance* inst = FindInstance(self)) {
        for (LoadedScript* s = FindScript(inst->primary_type()); s; s = FindScript(s->parent)) {
          if (const NativeFunction* nf = natives_->Find(s->name, method_name)) {
            RecordNative(s->name, method_name);
            return (*nf)(*this, self, args);
          }
        }
      }
      if (const NativeFunction* nf = natives_->Find(target.defining_type, method_name)) {
        RecordNative(target.defining_type, method_name);
        return (*nf)(*this, self, args);
      }
    }
    // No bound implementation. Rather than fail, return the neutral default of
    // the function's declared return type (false / 0 / "" / None). Scripts that
    // call an engine function not yet wired keep running with a sound value;
    // this is the uniform handler for the long tail of native declarations.
    WarnUnbound(target.defining_type, method_name);
    return DefaultForTypeName(target.script->pex.Str(target.fn->return_type));
  }
  call_stack_.push_back(target.defining_type);
  Value result = ExecuteFunction(target.script->pex, *target.script->object, *target.fn, self,
                                 std::move(args), *this, method_name);
  call_stack_.pop_back();
  return result;
}

bool VirtualMachine::SuspendCurrent() { return Fiber::YieldCurrent(); }

bool VirtualMachine::SuspendCurrentFor(f64 real_seconds, f64 game_days) {
  latent_ = {real_seconds, game_days};
  return Fiber::YieldCurrent();
}

LatentRequest VirtualMachine::TakeLatentRequest() {
  LatentRequest r = latent_;
  latent_ = {};
  return r;
}

Value VirtualMachine::Call(ObjectRef self, const std::string& method, std::vector<Value> args) {
  Instance* inst = FindInstance(self);
  if (!inst) {
    // Most refs (and every quest alias) have no attached script, so they are not
    // VM instances. Their methods are still engine-backed natives, so dispatch
    // against the common base types: this is what makes ObjectReference/Actor
    // calls (Is3DLoaded, GetDistance, ...) and ReferenceAlias.GetReference work
    // on a bare ref instead of silently returning None.
    if (self.handle != 0 && natives_) {
      static constexpr const char* kBaseTypes[] = {
          "Actor",        "ReferenceAlias", "LocationAlias", "Location",  "ObjectReference",
          "GlobalVariable", "Form",         "ActorBase"};
      for (const char* type : kBaseTypes) {
        if (const NativeFunction* nf = natives_->Find(type, method)) {
          RecordNative(type, method);
          return (*nf)(*this, self, args);
        }
      }
    }
    return Value();
  }
  Resolved r;
  if (!ResolveMethodAny(*inst, method, &r)) {
    WarnUnbound(inst->primary_type(), method);
    return Value();
  }
  return Invoke(r, self, std::move(args), method);
}

bool VirtualMachine::TryCall(ObjectRef self, const std::string& method, std::vector<Value> args) {
  Instance* inst = FindInstance(self);
  if (!inst) return false;
  Resolved r;
  if (!ResolveMethodAny(*inst, method, &r)) return false;  // no handler: silent
  Invoke(r, self, std::move(args), method);
  return true;
}

bool VirtualMachine::TryCallScript(ObjectRef self, const std::string& script_type,
                                   const std::string& method, std::vector<Value> args) {
  Instance* inst = FindInstance(self);
  if (!inst || !HasAttachedScript(self, script_type)) return false;
  Resolved resolved;
  if (!ResolveMethod(*inst, method, script_type, &resolved)) return false;
  Invoke(resolved, self, std::move(args), method);
  return true;
}

bool VirtualMachine::TryCallAll(ObjectRef self, const std::string& method,
                                const std::vector<Value>& args) {
  Instance* inst = FindInstance(self);
  if (!inst) return false;
  bool dispatched = false;
  // An event may attach another script; it joins only the next event.
  const std::vector<std::string> types = inst->types;
  for (const std::string& type : types) {
    inst = FindInstance(self);
    if (!inst) break;
    Resolved resolved;
    if (!ResolveMethod(*inst, method, type, &resolved)) continue;
    Invoke(resolved, self, args, method);
    dispatched = true;
  }
  return dispatched;
}

Value VirtualMachine::CallGlobal(const std::string& script_type, const std::string& function,
                                 std::vector<Value> args) {
  if (LoadedScript* s = FindScript(script_type)) {
    if (const Function* f = FindInState(s->pex, *s->object, "", function)) {
      Resolved r{s, f, s->name};
      return Invoke(r, ObjectRef{0}, std::move(args), function);
    }
  }
  if (natives_) {
    if (const NativeFunction* nf = natives_->Find(script_type, function)) {
      RecordNative(script_type, function);
      return (*nf)(*this, ObjectRef{0}, args);
    }
  }
  WarnUnbound(script_type, function);
  return Value();
}

void VirtualMachine::RecordNative(const std::string& type, const std::string& function) {
  ++native_call_count_;
  if (!native_trace_enabled_) return;
  if (native_trace_.size() >= kNativeTraceCap) native_trace_.erase(native_trace_.begin());
  native_trace_.push_back({type, function, native_call_count_});
}

Value VirtualMachine::CallMethod(ObjectRef self, const std::string& method,
                                 std::vector<Value> args) {
  return Call(self, method, std::move(args));
}

Value VirtualMachine::CallStatic(const std::string& script_type, const std::string& function,
                                 std::vector<Value> args) {
  return CallGlobal(script_type, function, std::move(args));
}

Value VirtualMachine::CallParent(ObjectRef self, const std::string& method,
                                 std::vector<Value> args) {
  Instance* inst = FindInstance(self);
  if (!inst) return Value();
  std::string current = call_stack_.empty() ? inst->primary_type() : call_stack_.back();
  LoadedScript* cs = FindScript(current);
  if (!cs || cs->parent.empty()) return Value();
  Resolved r;
  if (!ResolveMethod(*inst, method, cs->parent, &r)) {
    WarnUnbound(cs->parent, method);
    return Value();
  }
  return Invoke(r, self, std::move(args), method);
}

Value VirtualMachine::GetProperty(ObjectRef self, const std::string& property) {
  Instance* inst = FindInstance(self);
  if (!inst) return Value();
  LoadedScript* owner = nullptr;
  const Property* p = ResolveProperty(*inst, property, &owner);
  if (!p) return Value();
  if (p->is_auto()) {
    auto it = inst->members.find(owner->pex.Str(p->auto_var_name));
    return it == inst->members.end() ? Value() : it->second;
  }
  if (p->has_getter) {
    Resolved r{owner, &p->getter, owner->name};
    return Invoke(r, self, {}, "get");
  }
  return Value();
}

void VirtualMachine::SetProperty(ObjectRef self, const std::string& property, Value value) {
  Instance* inst = FindInstance(self);
  if (!inst) return;
  LoadedScript* owner = nullptr;
  const Property* p = ResolveProperty(*inst, property, &owner);
  if (!p) return;
  if (p->is_auto()) {
    inst->members[owner->pex.Str(p->auto_var_name)] = std::move(value);
    return;
  }
  if (p->has_setter) {
    Resolved r{owner, &p->setter, owner->name};
    std::vector<Value> args;
    args.push_back(std::move(value));
    Invoke(r, self, std::move(args), "set");
  }
}

Value* VirtualMachine::MemberVar(ObjectRef self, const std::string& name) {
  Instance* inst = FindInstance(self);
  if (!inst) return nullptr;
  auto it = inst->members.find(name);
  return it == inst->members.end() ? nullptr : &it->second;
}

std::vector<std::string> VirtualMachine::MemberNames(ObjectRef self) {
  std::vector<std::string> names;
  if (Instance* inst = FindInstance(self)) {
    names.reserve(inst->members.size());
    for (const auto& entry : inst->members) names.push_back(entry.first);
  }
  return names;
}

std::string VirtualMachine::CurrentState(ObjectRef self) {
  Instance* inst = FindInstance(self);
  return inst ? inst->state : "";
}

void VirtualMachine::GotoState(ObjectRef self, const std::string& state) {
  if (Instance* inst = FindInstance(self)) inst->state = state;
}

bool VirtualMachine::IsObjectOfType(ObjectRef obj, const std::string& type_name) {
  Instance* inst = FindInstance(obj);
  if (!inst) {
    // A bare reference (no scripted instance): the game resolves its kind from
    // records / runtime actor state so `placedRef as Actor` and the like work.
    return obj.handle != 0 && type_resolver_ && type_resolver_(obj, type_name);
  }
  std::string want = Lower(type_name);
  for (const std::string& type : inst->types) {
    std::string t = type;
    while (!t.empty()) {
      if (Lower(t) == want) return true;
      LoadedScript* s = FindScript(t);
      if (!s) break;
      t = s->parent;
    }
  }
  return false;
}

ArrayRef VirtualMachine::ArrayCreate(const std::string& element_type, i32 size) {
  arrays_.emplace_back(std::max(0, size), DefaultForTypeName(element_type));
  return ArrayRef{static_cast<u32>(arrays_.size())};
}

i32 VirtualMachine::ArrayLength(ArrayRef array) {
  return ArrayValid(array) ? static_cast<i32>(arrays_[array.id - 1].size()) : 0;
}

Value VirtualMachine::ArrayGet(ArrayRef array, i32 index) {
  if (!ArrayValid(array)) return Value();
  const auto& a = arrays_[array.id - 1];
  return index >= 0 && index < static_cast<i32>(a.size()) ? a[index] : Value();
}

void VirtualMachine::ArraySet(ArrayRef array, i32 index, Value value) {
  if (!ArrayValid(array)) return;
  auto& a = arrays_[array.id - 1];
  if (index >= 0 && index < static_cast<i32>(a.size())) a[index] = std::move(value);
}

i32 VirtualMachine::ArrayFind(ArrayRef array, const Value& value, i32 start) {
  if (!ArrayValid(array)) return -1;
  const auto& a = arrays_[array.id - 1];
  for (i32 i = std::max(0, start); i < static_cast<i32>(a.size()); ++i)
    if (a[i].Equals(value)) return i;
  return -1;
}

i32 VirtualMachine::ArrayRFind(ArrayRef array, const Value& value, i32 start) {
  if (!ArrayValid(array)) return -1;
  const auto& a = arrays_[array.id - 1];
  i32 from = start < 0 ? static_cast<i32>(a.size()) - 1
                       : std::min(start, static_cast<i32>(a.size()) - 1);
  for (i32 i = from; i >= 0; --i)
    if (a[i].Equals(value)) return i;
  return -1;
}

void VirtualMachine::ArrayAdd(ArrayRef array, const Value& value, i32 count) {
  if (!ArrayValid(array)) return;
  auto& a = arrays_[array.id - 1];
  for (i32 i = 0; i < count; ++i) a.push_back(value);
}

void VirtualMachine::ArrayInsert(ArrayRef array, i32 index, const Value& value) {
  if (!ArrayValid(array)) return;
  auto& a = arrays_[array.id - 1];
  if (index < 0 || index > static_cast<i32>(a.size())) return;
  a.insert(a.begin() + index, value);
}

void VirtualMachine::ArrayRemove(ArrayRef array, i32 index, i32 count) {
  if (!ArrayValid(array) || count <= 0) return;
  auto& a = arrays_[array.id - 1];
  if (index < 0 || index >= static_cast<i32>(a.size())) return;
  i32 end = std::min(index + count, static_cast<i32>(a.size()));
  a.erase(a.begin() + index, a.begin() + end);
}

void VirtualMachine::ArrayRemoveLast(ArrayRef array) {
  if (!ArrayValid(array)) return;
  auto& a = arrays_[array.id - 1];
  if (!a.empty()) a.pop_back();
}

void VirtualMachine::ArrayClear(ArrayRef array) {
  if (ArrayValid(array)) arrays_[array.id - 1].clear();
}

StructRef VirtualMachine::StructCreate(const std::string&) {
  structs_.emplace_back();
  return StructRef{static_cast<u32>(structs_.size())};
}

Value VirtualMachine::StructGet(StructRef instance, const std::string& member) {
  if (!StructValid(instance)) return Value();
  auto& s = structs_[instance.id - 1];
  auto it = s.find(member);
  return it == s.end() ? Value() : it->second;
}

void VirtualMachine::StructSet(StructRef instance, const std::string& member, Value value) {
  if (StructValid(instance)) structs_[instance.id - 1][member] = std::move(value);
}

void VirtualMachine::WarnUnbound(const std::string& type, const std::string& function) {
  std::string key = Lower(type) + "." + Lower(function);
  if (warned_.insert(key).second)
    RX_DEBUG("papyrus: unbound function {}.{} (returning None)", type, function);
}

}  // namespace rx::script::papyrus
