// pexrun: exercise the Papyrus interpreter.
//
//   pexrun selftest                       run built-in interpreter checks
//   pexrun <data_dir> <Script> <Function> link a shipped script and run one
//                                         no-argument function on a bare instance
//
// The embedded TestVm implements VmInterface with in-memory arrays and member
// storage; cross-object calls and properties return None. This isolates the
// interpreter so its opcode handling can be checked end to end.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <utility>
#include <string>
#include <unordered_map>
#include <vector>

#include "asset/vfs.h"
#include "bethesda/archive.h"
#include "script/papyrus/interpreter.h"
#include "script/papyrus/native.h"
#include "script/papyrus/pex.h"
#include "script/papyrus/value.h"
#include "script/papyrus/vm.h"
#include "script/papyrus_guest.h"
#include "script/host/bridge.h"
#include "script/host/clr_host.h"
#include "script/host/guest_bridge.h"
#include "script/games/skyrim/skyrim_natives.h"

namespace {

using namespace rec;
using namespace rec::script::papyrus;

// Minimal VM backing for the interpreter: one instance's member variables, a
// flat array heap, and a current state string. Calls and properties are stubs.
class TestVm : public VmInterface {
 public:
  std::unordered_map<std::string, Value> members;
  std::vector<std::pair<std::string, Value>> property_sets;  // recorded for tests

  Value CallMethod(ObjectRef, const std::string&, std::vector<Value>) override { return Value(); }
  Value CallStatic(const std::string&, const std::string&, std::vector<Value>) override {
    return Value();
  }
  Value CallParent(ObjectRef, const std::string&, std::vector<Value>) override { return Value(); }
  Value GetProperty(ObjectRef, const std::string&) override { return Value(); }
  void SetProperty(ObjectRef, const std::string& p, Value v) override {
    property_sets.emplace_back(p, std::move(v));
  }
  Value* MemberVar(ObjectRef, const std::string& name) override {
    auto it = members.find(name);
    return it == members.end() ? nullptr : &it->second;
  }
  std::string CurrentState(ObjectRef) override { return state_; }
  void GotoState(ObjectRef, const std::string& s) override { state_ = s; }
  bool IsObjectOfType(ObjectRef, const std::string&) override { return true; }

  ArrayRef ArrayCreate(const std::string&, i32 size) override {
    arrays_.emplace_back(std::max(0, size));
    return ArrayRef{static_cast<u32>(arrays_.size())};  // 1-based; 0 = None
  }
  i32 ArrayLength(ArrayRef a) override {
    return Valid(a) ? static_cast<i32>(arrays_[a.id - 1].size()) : 0;
  }
  Value ArrayGet(ArrayRef a, i32 i) override {
    return Valid(a) && i >= 0 && i < (i32)arrays_[a.id - 1].size() ? arrays_[a.id - 1][i] : Value();
  }
  void ArraySet(ArrayRef a, i32 i, Value v) override {
    if (Valid(a) && i >= 0 && i < (i32)arrays_[a.id - 1].size()) arrays_[a.id - 1][i] = std::move(v);
  }
  i32 ArrayFind(ArrayRef a, const Value& v, i32 start) override {
    if (!Valid(a)) return -1;
    const auto& arr = arrays_[a.id - 1];
    for (i32 i = std::max(0, start); i < (i32)arr.size(); ++i)
      if (arr[i].Equals(v)) return i;
    return -1;
  }
  i32 ArrayRFind(ArrayRef a, const Value& v, i32 start) override {
    if (!Valid(a)) return -1;
    const auto& arr = arrays_[a.id - 1];
    i32 from = start < 0 ? (i32)arr.size() - 1 : std::min(start, (i32)arr.size() - 1);
    for (i32 i = from; i >= 0; --i)
      if (arr[i].Equals(v)) return i;
    return -1;
  }

 private:
  bool Valid(ArrayRef a) const { return a.id != 0 && a.id <= arrays_.size(); }
  std::vector<std::vector<Value>> arrays_;
  std::string state_;
};

// Builds a tiny .pex in memory so the interpreter runs over a known program.
struct Builder {
  PexFile pex;
  Object obj;
  StringIndex S(const std::string& s) {
    for (StringIndex i = 0; i < pex.string_table.size(); ++i)
      if (pex.string_table[i] == s) return i;
    pex.string_table.push_back(s);
    return static_cast<StringIndex>(pex.string_table.size() - 1);
  }
  VariableData Id(const std::string& n) { return {VariableData::Type::kIdentifier, S(n), 0, 0, false}; }
  VariableData IntV(i32 v) {
    VariableData d;
    d.type = VariableData::Type::kInteger;
    d.int_value = v;
    return d;
  }
  VariableData FloatV(f32 v) {
    VariableData d;
    d.type = VariableData::Type::kFloat;
    d.float_value = v;
    return d;
  }
  VariableData StrV(const std::string& s) {
    return {VariableData::Type::kString, S(s), 0, 0, false};
  }
};

Instruction Make(Op op, std::vector<VariableData> args) { return {op, std::move(args), {}}; }

int SelfTest() {
  int failures = 0;
  auto check = [&](const char* what, bool ok) {
    std::printf("  %-40s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
  };

  // Value coercion sanity.
  check("int->float coerce", Value::Int(3).ToFloat() == 3.0f);
  check("float->int trunc", Value::Float(3.9f).ToInt() == 3);
  check("string->bool empty", Value::Str("").ToBool() == false);
  check("bool tostring", Value::Bool(true).ToString() == "True");
  check("numeric equals promote", Value::Int(2).Equals(Value::Float(2.0f)));
  check("compare lt", Value::Int(1).Compare(Value::Int(2)) < 0);

  // A summation loop returning a string: exercises assign, iadd, cmp_le, a
  // backward jmpt, strcat and return over real register resolution.
  Builder b;
  Function fn;
  fn.return_type = b.S("String");
  fn.params.push_back({b.S("n"), b.S("Int")});
  fn.locals.push_back({b.S("acc"), b.S("Int")});
  fn.locals.push_back({b.S("i"), b.S("Int")});
  fn.locals.push_back({b.S("t"), b.S("Bool")});
  fn.locals.push_back({b.S("r"), b.S("String")});
  fn.code = {
      Make(Op::kAssign, {b.Id("acc"), b.IntV(0)}),           // 0
      Make(Op::kAssign, {b.Id("i"), b.IntV(1)}),             // 1
      Make(Op::kIAdd, {b.Id("acc"), b.Id("acc"), b.Id("i")}),  // 2 loop
      Make(Op::kIAdd, {b.Id("i"), b.Id("i"), b.IntV(1)}),    // 3
      Make(Op::kCmpLe, {b.Id("t"), b.Id("i"), b.Id("n")}),   // 4
      Make(Op::kJmpT, {b.Id("t"), b.IntV(-3)}),              // 5 -> 2
      Make(Op::kStrCat, {b.Id("r"), b.StrV("sum="), b.Id("acc")}),  // 6
      Make(Op::kReturn, {b.Id("r")}),                        // 7
  };
  TestVm vm;
  Value out = ExecuteFunction(b.pex, b.obj, fn, ObjectRef{1}, {Value::Int(5)}, vm);
  check("sum loop returns sum=15", out.ToString() == "sum=15");

  // Arrays through the VM hooks.
  Function af;
  af.return_type = b.S("Int");
  af.locals.push_back({b.S("arr"), b.S("Int[]")});
  af.locals.push_back({b.S("len"), b.S("Int")});
  af.code = {
      Make(Op::kArrayCreate, {b.Id("arr"), b.IntV(3)}),
      Make(Op::kArraySetElement, {b.Id("arr"), b.IntV(0), b.IntV(10)}),
      Make(Op::kArraySetElement, {b.Id("arr"), b.IntV(2), b.IntV(30)}),
      Make(Op::kArrayLength, {b.Id("len"), b.Id("arr")}),
      Make(Op::kReturn, {b.Id("len")}),
  };
  Value alen = ExecuteFunction(b.pex, b.obj, af, ObjectRef{1}, {}, vm);
  check("array length is 3", alen.ToInt() == 3);

  std::printf("%s (%d failures)\n", failures ? "SELFTEST FAILED" : "SELFTEST PASSED", failures);
  return failures ? 1 : 0;
}

const Function* FindFunction(const PexFile& pex, const Object& obj, const std::string& name,
                             const Object** owner) {
  for (const State& s : obj.states)
    for (const NamedFunction& nf : s.functions)
      if (pex.Str(nf.name) == name) {
        *owner = &obj;
        return &nf.function;
      }
  return nullptr;
}

int RunReal(const std::string& data_dir, const std::string& script, const std::string& function) {
  asset::Vfs vfs;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(data_dir, ec))
    if (auto p = bethesda::OpenArchive(entry.path().string())) vfs.Mount(std::move(p));

  auto blob = vfs.Read("scripts/" + script + ".pex");
  if (!blob) {
    std::printf("not found: scripts/%s.pex\n", script.c_str());
    return 1;
  }
  PexFile pex;
  if (!ParsePex(ByteSpan(blob->data(), blob->size()), &pex) || pex.objects.empty()) {
    std::printf("parse failed\n");
    return 1;
  }
  const Object& obj = pex.objects[0];
  const Object* owner = nullptr;
  const Function* fn = FindFunction(pex, obj, function, &owner);
  if (!fn) {
    std::printf("no function %s in %s\n", function.c_str(), script.c_str());
    return 1;
  }
  if (fn->is_native) {
    std::printf("%s is native (no bytecode)\n", function.c_str());
    return 1;
  }

  TestVm vm;
  for (const MemberVariable& v : obj.variables) {
    Value init;
    switch (v.initial_value.type) {
      case VariableData::Type::kInteger:
        init = Value::Int(v.initial_value.int_value);
        break;
      case VariableData::Type::kFloat:
        init = Value::Float(v.initial_value.float_value);
        break;
      case VariableData::Type::kBool:
        init = Value::Bool(v.initial_value.bool_value);
        break;
      case VariableData::Type::kString:
        init = Value::Str(pex.Str(v.initial_value.string_index));
        break;
      default:
        break;
    }
    vm.members[pex.Str(v.name)] = init;
  }

  Value out = ExecuteFunction(pex, *owner, *fn, ObjectRef{1}, {}, vm);
  std::printf("%s.%s() = %s\n", script.c_str(), function.c_str(), out.ToString().c_str());
  std::printf("  %zu property sets, final state \"%s\"\n", vm.property_sets.size(),
              vm.CurrentState(ObjectRef{1}).c_str());
  return 0;
}

// Exercises the full VM (load, instantiate, dispatch, properties, states)
// against the shipped TrapBase script.
int VmTest(const std::string& data_dir) {
  asset::Vfs vfs;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(data_dir, ec))
    if (auto p = bethesda::OpenArchive(entry.path().string())) vfs.Mount(std::move(p));
  auto blob = vfs.Read("scripts/TrapBase.pex");
  if (!blob) {
    std::printf("TrapBase.pex not found\n");
    return 1;
  }

  NativeRegistry natives;  // empty: base-class natives stay unbound (return None)
  VirtualMachine vm(&natives);
  std::string type = vm.LoadScript(ByteSpan(blob->data(), blob->size()));
  ObjectRef inst = vm.CreateInstance(type);

  int failures = 0;
  auto check = [&](const char* what, bool ok) {
    std::printf("  %-40s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
  };

  check("loaded TrapBase", type == "TrapBase");
  check("instance alive", vm.IsAlive(inst));
  check("TypeOf is TrapBase", vm.TypeOf(inst) == "TrapBase");
  check("auto property damage defaults 0", vm.GetProperty(inst, "damage").ToInt() == 0);
  vm.SetProperty(inst, "damage", Value::Int(50));
  check("auto property damage set to 50", vm.GetProperty(inst, "damage").ToInt() == 50);
  check("auto property loop defaults false", vm.GetProperty(inst, "loop").ToBool() == false);

  vm.Call(inst, "GotoState", {Value::Str("On")});
  check("GotoState changed state to On", vm.CurrentState(inst) == "On");

  check("is-a TrapBase", vm.IsObjectOfType(inst, "TrapBase"));
  check("is-a objectReference (parent)", vm.IsObjectOfType(inst, "objectReference"));
  check("not-a Actor", !vm.IsObjectOfType(inst, "Actor"));

  // State-gated dispatch: onActivate lives in the Idle state. Calling it from
  // another state resolves to nothing and returns None without error.
  Value gated = vm.Call(inst, "onActivate", {Value::Object({0})});
  check("state-gated onActivate returns None", gated.is_none());

  // From the Idle state it runs the real 44-instruction body to completion.
  vm.Call(inst, "GotoState", {Value::Str("Idle")});
  vm.Call(inst, "onActivate", {Value::Object({0})});
  check("Idle.onActivate ran without error", true);

  std::printf("%s (%d failures)\n", failures ? "VMTEST FAILED" : "VMTEST PASSED", failures);
  return failures ? 1 : 0;
}

// Builds a one-object script in memory and returns the PexFile.
PexFile MakeScript(Builder& b) {
  b.pex.objects.push_back(b.obj);
  return std::move(b.pex);
}

struct TickerScripts {
  PexFile form;
  PexFile ticker;
};

// A minimal base script declaring the timer native, and a Ticker subclass that
// registers for one update and counts OnUpdate firings into a Count property.
// Shared by the guest and host tests.
TickerScripts BuildTickerScripts() {
  Builder fb;
  fb.obj.name = fb.S("Form");
  fb.obj.parent_class = fb.S("");
  {
    Function reg;
    reg.return_type = fb.S("");
    reg.is_native = true;
    reg.params.push_back({fb.S("afInterval"), fb.S("Float")});
    State def;
    def.name = fb.S("");
    def.functions.push_back({fb.S("RegisterForSingleUpdate"), std::move(reg)});
    fb.obj.states.push_back(std::move(def));
  }

  Builder tb;
  tb.obj.name = tb.S("Ticker");
  tb.obj.parent_class = tb.S("Form");
  {
    MemberVariable count;
    count.name = tb.S("::Count_var");
    count.type = tb.S("Int");
    count.initial_value = tb.IntV(0);
    tb.obj.variables.push_back(count);

    Property prop;  // auto Int property Count, backed by ::Count_var
    prop.name = tb.S("Count");
    prop.type = tb.S("Int");
    prop.flags = 0x7;  // readable | writable | auto
    prop.auto_var_name = tb.S("::Count_var");
    tb.obj.properties.push_back(prop);

    Function begin;
    begin.return_type = tb.S("");
    Instruction call;
    call.op = Op::kCallMethod;
    call.args = {tb.Id("RegisterForSingleUpdate"), tb.Id("self"), tb.Id("::NoneVar")};
    call.var_args = {tb.FloatV(0.5f)};
    begin.code = {call};

    Function on_update;
    on_update.return_type = tb.S("");
    on_update.code = {
        Make(Op::kIAdd, {tb.Id("::Count_var"), tb.Id("::Count_var"), tb.IntV(1)}),
        Make(Op::kReturn, {}),
    };

    State def;
    def.name = tb.S("");
    def.functions.push_back({tb.S("begin"), std::move(begin)});
    def.functions.push_back({tb.S("OnUpdate"), std::move(on_update)});
    tb.obj.states.push_back(std::move(def));
  }

  return {MakeScript(fb), MakeScript(tb)};
}

// Exercises the threaded guest end to end: the dedicated VM thread, FIFO
// command ordering, native dispatch through the inheritance chain, the update
// scheduler and OnUpdate event firing, plus cross-thread reads via futures.
int GuestTest() {
  using rec::script::PapyrusGuest;

  TickerScripts scripts = BuildTickerScripts();
  PexFile form = std::move(scripts.form);
  PexFile ticker = std::move(scripts.ticker);

  PapyrusGuest guest(bethesda::Game::kSkyrimSe);
  guest.Start();
  guest.SubmitFor([f = std::move(form)](VirtualMachine& vm) mutable {
        return vm.AddScript(std::move(f));
      }).get();
  guest.SubmitFor([t = std::move(ticker)](VirtualMachine& vm) mutable {
        return vm.AddScript(std::move(t));
      }).get();
  ObjectRef inst = guest.CreateInstance("Ticker").get();

  auto read_count = [&] {
    return guest
        .SubmitFor([inst](VirtualMachine& vm) {
          Value* m = vm.MemberVar(inst, "::Count_var");
          return m ? m->ToInt() : -1;
        })
        .get();
  };

  int failures = 0;
  auto check = [&](const char* what, bool ok) {
    std::printf("  %-40s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
  };

  check("instance created on guest thread", guest.SubmitFor([inst](VirtualMachine& vm) {
                                                   return vm.IsAlive(inst);
                                                 }).get());
  check("count starts at 0", read_count() == 0);

  guest.RaiseEvent(inst, "begin");  // schedules a single update 0.5s out
  guest.Tick(0.4f);
  check("no update before 0.5s", read_count() == 0);
  guest.Tick(0.2f);  // clock crosses 0.5s
  check("OnUpdate fired once by 0.6s", read_count() == 1);
  guest.Tick(1.0f);  // single-shot does not refire
  check("single update does not repeat", read_count() == 1);

  guest.Stop();
  std::printf("%s (%d failures)\n", failures ? "GUESTTEST FAILED" : "GUESTTEST PASSED", failures);
  return failures ? 1 : 0;
}

// Drives the full two-worlds path: boots the .NET CoreCLR host, hands it a
// bridge to a live Papyrus guest, and lets managed C# create an instance, run
// its update loop and read a property back. Verifies the guest state the
// managed code produced. Skips cleanly (rc 0) when no .NET runtime is present.
int HostTest(const std::string& runtime_config, const std::string& assembly) {
  using rec::script::PapyrusGuest;

  TickerScripts scripts = BuildTickerScripts();
  PapyrusGuest guest(bethesda::Game::kSkyrimSe);
  guest.Start();
  guest.SubmitFor([f = std::move(scripts.form)](VirtualMachine& vm) mutable {
        return vm.AddScript(std::move(f));
      }).get();
  guest.SubmitFor([t = std::move(scripts.ticker)](VirtualMachine& vm) mutable {
        return vm.AddScript(std::move(t));
      }).get();

  rec::script::host::GuestBridge bridge = rec::script::host::MakeGuestBridge(guest);
  rec::script::host::ClrHost host;
  if (!host.Initialize(/*dotnet_root=*/"", runtime_config, assembly,
                       "Recreation.ScriptHost, Recreation.Scripting", "Main")) {
    std::printf("HOSTTEST SKIPPED (no .NET runtime / entrypoint)\n");
    guest.Stop();
    return 0;
  }

  int managed_result = host.Invoke(&bridge);
  std::printf("[native] managed Main returned %d\n", managed_result);

  int guest_count = guest
                        .SubmitFor([&](VirtualMachine& vm) {
                          // The instance the managed side created has handle 1.
                          Value* m = vm.MemberVar(ObjectRef{1}, "::Count_var");
                          return m ? m->ToInt() : -1;
                        })
                        .get();

  host.Shutdown();
  guest.Stop();

  bool ok = managed_result == 1 && guest_count == 1;
  std::printf("guest-side Count = %d\n%s\n", guest_count,
              ok ? "HOSTTEST PASSED" : "HOSTTEST FAILED");
  return ok ? 0 : 1;
}

// A native method on a synthetic script (is_native, no body).
NamedFunction NativeMethod(Builder& b, const char* name,
                           std::vector<std::pair<const char*, const char*>> params) {
  Function fn;
  fn.is_native = true;
  fn.return_type = b.S("");
  for (auto& p : params) fn.params.push_back({b.S(p.first), b.S(p.second)});
  return {b.S(name), std::move(fn)};
}

// Engine bindings stub with known values, so native results are checkable.
struct TestBindings : rec::script::skyrim::SkyrimBindings {
  ObjectRef GetPlayer() override { return ObjectRef{0x14}; }
  f32 GetPositionX(ObjectRef) override { return 1.0f; }
  f32 GetActorValue(ObjectRef, const std::string& av) override {
    return (av == "Health" || av == "health") ? 100.0f : 0.0f;
  }
  i32 GetLevel(ObjectRef) override { return 5; }
  bool IsDead(ObjectRef) override { return false; }
  bool IsInCombat(ObjectRef) override { return true; }
  i32 GetItemCount(ObjectRef, ObjectRef) override { return 3; }
  std::unordered_map<rec::u64, i32> stages;
  i32 GetStage(ObjectRef q) override { return stages.count(q.handle) ? stages[q.handle] : 0; }
  void SetStage(ObjectRef q, i32 s) override { stages[q.handle] = s; }
};

// Validates the Skyrim native surface: fully-implemented Math/Utility globals,
// and engine-bound method natives dispatched (including through inheritance) to
// the bindings.
int SkyrimTest() {
  TestBindings bindings;
  NativeRegistry natives;
  rec::script::skyrim::RegisterSkyrimNatives(natives, &bindings);
  VirtualMachine vm(&natives);

  // ObjectReference (native GetPositionX) and Actor extends it (native AV ops).
  Builder ob;
  ob.obj.name = ob.S("ObjectReference");
  ob.obj.parent_class = ob.S("");
  {
    State def;
    def.name = ob.S("");
    def.functions.push_back(NativeMethod(ob, "GetPositionX", {}));
    def.functions.push_back(NativeMethod(ob, "GetItemCount", {{"akItem", "Form"}}));
    ob.obj.states.push_back(std::move(def));
  }
  vm.AddScript(MakeScript(ob));

  Builder ac;
  ac.obj.name = ac.S("Actor");
  ac.obj.parent_class = ac.S("ObjectReference");
  {
    State def;
    def.name = ac.S("");
    def.functions.push_back(NativeMethod(ac, "GetActorValue", {{"asValueName", "String"}}));
    def.functions.push_back(NativeMethod(ac, "GetLevel", {}));
    def.functions.push_back(NativeMethod(ac, "IsDead", {}));
    def.functions.push_back(NativeMethod(ac, "IsInCombat", {}));
    ac.obj.states.push_back(std::move(def));
  }
  vm.AddScript(MakeScript(ac));
  ObjectRef actor = vm.CreateInstance("Actor");

  int failures = 0;
  auto check = [&](const char* what, bool ok) {
    std::printf("  %-40s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
  };
  auto nearly = [](f32 a, f32 b) { return std::fabs(a - b) < 0.001f; };

  // Globals (no script needed; resolved straight from the native table).
  check("Math.Sqrt(16) == 4", nearly(vm.CallGlobal("Math", "Sqrt", {Value::Float(16)}).ToFloat(), 4));
  check("Math.Abs(-3) == 3", nearly(vm.CallGlobal("Math", "Abs", {Value::Float(-3)}).ToFloat(), 3));
  check("Math.Floor(3.9) == 3", vm.CallGlobal("Math", "Floor", {Value::Float(3.9f)}).ToInt() == 3);
  check("Math.Ceiling(3.1) == 4",
        vm.CallGlobal("Math", "Ceiling", {Value::Float(3.1f)}).ToInt() == 4);
  check("Math.Pow(2,10) == 1024",
        nearly(vm.CallGlobal("Math", "Pow", {Value::Float(2), Value::Float(10)}).ToFloat(), 1024));
  check("Math.Sin(90deg) == 1", nearly(vm.CallGlobal("Math", "Sin", {Value::Float(90)}).ToFloat(), 1));
  check("Math.Cos(0deg) == 1", nearly(vm.CallGlobal("Math", "Cos", {Value::Float(0)}).ToFloat(), 1));
  check("Utility.RandomInt(7,7) == 7",
        vm.CallGlobal("Utility", "RandomInt", {Value::Int(7), Value::Int(7)}).ToInt() == 7);
  check("Game.GetPlayer() == 0x14",
        vm.CallGlobal("Game", "GetPlayer", {}).as_object().handle == 0x14);

  // Method natives via dispatch (GetPositionX inherited from ObjectReference).
  check("Actor.GetActorValue(Health) == 100",
        nearly(vm.Call(actor, "GetActorValue", {Value::Str("Health")}).ToFloat(), 100));
  check("Actor.GetLevel() == 5", vm.Call(actor, "GetLevel", {}).ToInt() == 5);
  check("Actor.IsDead() == false", vm.Call(actor, "IsDead", {}).ToBool() == false);
  check("inherited ObjectReference.GetPositionX() == 1",
        nearly(vm.Call(actor, "GetPositionX", {}).ToFloat(), 1));
  check("Actor.IsInCombat() == true (bindings)", vm.Call(actor, "IsInCombat", {}).ToBool());
  check("inherited GetItemCount() == 3",
        vm.Call(actor, "GetItemCount", {Value::Object({0})}).ToInt() == 3);

  // Quest natives dispatch through the VM to the bindings' quest system.
  Builder qb;
  qb.obj.name = qb.S("Quest");
  qb.obj.parent_class = qb.S("");
  {
    State def;
    def.name = qb.S("");
    def.functions.push_back(NativeMethod(qb, "SetStage", {{"aiStage", "Int"}}));
    def.functions.push_back(NativeMethod(qb, "GetStage", {}));
    qb.obj.states.push_back(std::move(def));
  }
  vm.AddScript(MakeScript(qb));
  ObjectRef quest = vm.CreateInstance("Quest");
  vm.Call(quest, "SetStage", {Value::Int(15)});
  check("Quest.SetStage/GetStage dispatch == 15", vm.Call(quest, "GetStage", {}).ToInt() == 15);

  std::printf("%s (%d failures)\n", failures ? "SKYRIMTEST FAILED" : "SKYRIMTEST PASSED", failures);
  return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "selftest") return SelfTest();
  if (argc == 2 && std::string(argv[1]) == "skyrimtest") return SkyrimTest();
  if (argc == 2 && std::string(argv[1]) == "guesttest") return GuestTest();
  if (argc == 3 && std::string(argv[1]) == "vmtest") return VmTest(argv[2]);
  if (argc == 4 && std::string(argv[1]) == "hosttest") return HostTest(argv[2], argv[3]);
  if (argc == 4) return RunReal(argv[1], argv[2], argv[3]);
  std::fprintf(stderr,
               "usage: %s [selftest|skyrimtest|guesttest]\n"
               "       %s vmtest <data_dir>\n"
               "       %s hosttest <runtimeconfig.json> <assembly.dll>\n"
               "       %s <data_dir> <Script> <Function>\n",
               argv[0], argv[0], argv[0], argv[0]);
  return 2;
}
