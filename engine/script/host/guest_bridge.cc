#include "script/host/guest_bridge.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "script/papyrus/value.h"
#include "script/papyrus_guest.h"

namespace rx::script::host {
namespace {

using papyrus::ArrayRef;
using papyrus::ObjectRef;
using papyrus::Value;
using papyrus::ValueType;
using papyrus::VirtualMachine;

BridgeContext& Ctx(void* ctx) { return *static_cast<BridgeContext*>(ctx); }
PapyrusGuest& Guest(void* ctx) { return *Ctx(ctx).guest; }

// Marshals a VM value out to managed code. kString points at a thread-local
// scratch that stays valid until the next bridge call on this thread, which is
// all managed code needs: it copies the string into a managed string before
// making another call. The boundary is synchronous and single-threaded per host.
ApiValue ToApi(const Value& v) {
  thread_local std::string scratch;
  ApiValue out{};
  switch (v.type()) {
    case ValueType::kInt:
      out.kind = ApiKind::kInt;
      out.i = v.as_int();
      break;
    case ValueType::kFloat:
      out.kind = ApiKind::kFloat;
      out.f = v.as_float();
      break;
    case ValueType::kBool:
      out.kind = ApiKind::kBool;
      out.i = v.as_bool() ? 1 : 0;
      break;
    case ValueType::kString:
      out.kind = ApiKind::kString;
      scratch = v.as_string();
      out.s = scratch.c_str();
      break;
    case ValueType::kObject:
      out.kind = ApiKind::kObject;
      out.h = v.as_object().handle;
      break;
    case ValueType::kArray:
      out.kind = ApiKind::kArray;
      out.h = v.as_array().id;
      break;
    default:
      out.kind = ApiKind::kNone;
      break;
  }
  return out;
}

// Marshals a managed argument in to a VM value. Strings are copied; the borrowed
// pointer need only survive the call.
Value FromApi(const ApiValue& v) {
  switch (v.kind) {
    case ApiKind::kInt:
      return Value::Int(v.i);
    case ApiKind::kFloat:
      return Value::Float(v.f);
    case ApiKind::kBool:
      return Value::Bool(v.i != 0);
    case ApiKind::kString:
      return Value::Str(v.s ? std::string(v.s) : std::string());
    case ApiKind::kObject:
      return Value::Object(ObjectRef{v.h});
    case ApiKind::kArray:
      return Value::Array(ArrayRef{static_cast<u32>(v.h)});
    default:
      return Value();
  }
}

std::vector<Value> FromApiArgs(const ApiValue* args, std::int32_t argc) {
  std::vector<Value> out;
  out.reserve(argc > 0 ? static_cast<size_t>(argc) : 0);
  for (std::int32_t i = 0; i < argc; ++i) out.push_back(FromApi(args[i]));
  return out;
}

std::int32_t IsScriptLoaded(void* ctx, const char* type) {
  std::string name = type ? type : "";
  return Guest(ctx).Dispatch([name](VirtualMachine& vm) { return vm.HasScript(name) ? 1 : 0; });
}

std::int32_t LoadScript(void* ctx, const char* type) {
  std::string name = type ? type : "";
  auto& loader = Ctx(ctx).loader;
  if (loader) return loader(name) ? 1 : 0;
  return IsScriptLoaded(ctx, type);
}

std::uint64_t CreateInstance(void* ctx, const char* type) {
  std::string name = type ? type : "";
  return Guest(ctx).Dispatch([name](VirtualMachine& vm) { return vm.CreateInstance(name); }).handle;
}

std::int32_t TypeOf(void* ctx, std::uint64_t handle, char* buf, std::int32_t buf_len) {
  std::string type =
      Guest(ctx).Dispatch([handle](VirtualMachine& vm) { return vm.TypeOf(ObjectRef{handle}); });
  if (buf && buf_len > 0) {
    std::int32_t n = std::min<std::int32_t>(buf_len - 1, static_cast<std::int32_t>(type.size()));
    std::memcpy(buf, type.data(), static_cast<size_t>(n));
    buf[n] = '\0';
  }
  return static_cast<std::int32_t>(type.size());
}

void CallGlobal(void* ctx, const char* type, const char* func, const ApiValue* args,
                std::int32_t argc, ApiValue* result) {
  std::string t = type ? type : "";
  std::string f = func ? func : "";
  std::vector<Value> a = FromApiArgs(args, argc);
  Value r = Guest(ctx).Dispatch([t, f, a = std::move(a)](VirtualMachine& vm) mutable {
    return vm.CallGlobal(t, f, std::move(a));
  });
  if (result) *result = ToApi(r);
}

void CallMethod(void* ctx, std::uint64_t self, const char* func, const ApiValue* args,
                std::int32_t argc, ApiValue* result) {
  std::string f = func ? func : "";
  std::vector<Value> a = FromApiArgs(args, argc);
  Value r = Guest(ctx).Dispatch([self, f, a = std::move(a)](VirtualMachine& vm) mutable {
    return vm.Call(ObjectRef{self}, f, std::move(a));
  });
  if (result) *result = ToApi(r);
}

void GetProperty(void* ctx, std::uint64_t self, const char* name, ApiValue* result) {
  std::string p = name ? name : "";
  Value r = Guest(ctx).Dispatch(
      [self, p](VirtualMachine& vm) { return vm.GetProperty(ObjectRef{self}, p); });
  if (result) *result = ToApi(r);
}

void SetProperty(void* ctx, std::uint64_t self, const char* name, ApiValue value) {
  std::string p = name ? name : "";
  Value v = FromApi(value);
  Guest(ctx).Dispatch([self, p, v = std::move(v)](VirtualMachine& vm) mutable {
    vm.SetProperty(ObjectRef{self}, p, std::move(v));
    return 0;
  });
}

void Tick(void* ctx, float dt) { Guest(ctx).Tick(dt); }

}  // namespace

ScriptBridge MakeScriptBridge(BridgeContext& ctx) {
  ScriptBridge bridge{};
  bridge.ctx = &ctx;
  bridge.is_script_loaded = &IsScriptLoaded;
  bridge.load_script = &LoadScript;
  bridge.create_instance = &CreateInstance;
  bridge.type_of = &TypeOf;
  bridge.call_global = &CallGlobal;
  bridge.call_method = &CallMethod;
  bridge.get_property = &GetProperty;
  bridge.set_property = &SetProperty;
  bridge.tick = &Tick;
  return bridge;
}

}  // namespace rx::script::host
