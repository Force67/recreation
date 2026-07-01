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
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <utility>
#include <string>
#include <unordered_map>
#include <vector>

#include "asset/vfs.h"
#include "bethesda/archive.h"
#include "script/papyrus/alias_handle.h"
#include "script/papyrus/interpreter.h"
#include "script/papyrus/native.h"
#include "script/papyrus/pex.h"
#include "script/papyrus/value.h"
#include "script/papyrus/vm.h"
#include "script/papyrus_guest.h"
#include "script/host/bridge.h"
#include "script/host/clr_host.h"
#include "script/host/guest_bridge.h"
#include "script/host/managed_host.h"
#include "script/games/skyrim/skyrim_bindings.h"
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
  void ArrayAdd(ArrayRef a, const Value& v, i32 count) override {
    if (Valid(a))
      for (i32 i = 0; i < count; ++i) arrays_[a.id - 1].push_back(v);
  }
  void ArrayInsert(ArrayRef a, i32 i, const Value& v) override {
    if (Valid(a) && i >= 0 && i <= (i32)arrays_[a.id - 1].size())
      arrays_[a.id - 1].insert(arrays_[a.id - 1].begin() + i, v);
  }
  void ArrayRemove(ArrayRef a, i32 i, i32 c) override {
    if (!Valid(a) || c <= 0 || i < 0 || i >= (i32)arrays_[a.id - 1].size()) return;
    auto& arr = arrays_[a.id - 1];
    arr.erase(arr.begin() + i, arr.begin() + std::min(i + c, (i32)arr.size()));
  }
  void ArrayRemoveLast(ArrayRef a) override {
    if (Valid(a) && !arrays_[a.id - 1].empty()) arrays_[a.id - 1].pop_back();
  }
  void ArrayClear(ArrayRef a) override {
    if (Valid(a)) arrays_[a.id - 1].clear();
  }
  StructRef StructCreate(const std::string&) override {
    structs_.emplace_back();
    return StructRef{static_cast<u32>(structs_.size())};
  }
  Value StructGet(StructRef s, const std::string& m) override {
    if (s.id == 0 || s.id > structs_.size()) return Value();
    auto it = structs_[s.id - 1].find(m);
    return it == structs_[s.id - 1].end() ? Value() : it->second;
  }
  void StructSet(StructRef s, const std::string& m, Value v) override {
    if (s.id != 0 && s.id <= structs_.size()) structs_[s.id - 1][m] = std::move(v);
  }

 private:
  bool Valid(ArrayRef a) const { return a.id != 0 && a.id <= arrays_.size(); }
  std::vector<std::vector<Value>> arrays_;
  std::vector<std::unordered_map<std::string, Value>> structs_;
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

  // Fallout 4/76 opcodes: resizable arrays (add/insert/removelast/remove).
  Function rf;
  rf.return_type = b.S("Int");
  rf.locals.push_back({b.S("arr"), b.S("Int[]")});
  rf.locals.push_back({b.S("rlen"), b.S("Int")});
  rf.code = {
      Make(Op::kArrayCreate, {b.Id("arr"), b.IntV(0)}),
      Make(Op::kArrayAdd, {b.Id("arr"), b.IntV(5), b.IntV(3)}),       // [5,5,5]
      Make(Op::kArrayAdd, {b.Id("arr"), b.IntV(9), b.IntV(1)}),       // [5,5,5,9]
      Make(Op::kArrayInsert, {b.Id("arr"), b.IntV(7), b.IntV(0)}),    // [7,5,5,5,9]
      Make(Op::kArrayRemoveLast, {b.Id("arr")}),                      // [7,5,5,5]
      Make(Op::kArrayRemove, {b.Id("arr"), b.IntV(1), b.IntV(2)}),    // [7,5]
      Make(Op::kArrayLength, {b.Id("rlen"), b.Id("arr")}),
      Make(Op::kReturn, {b.Id("rlen")}),
  };
  check("fallout resizable array ops -> len 2",
        ExecuteFunction(b.pex, b.obj, rf, ObjectRef{1}, {}, vm).ToInt() == 2);

  // Fallout opcodes: structs (create/set/get).
  Function sf;
  sf.return_type = b.S("Int");
  sf.locals.push_back({b.S("s"), b.S("MyStruct")});
  sf.locals.push_back({b.S("sx"), b.S("Int")});
  sf.code = {
      Make(Op::kStructCreate, {b.Id("s")}),
      Make(Op::kStructSet, {b.Id("s"), b.Id("field"), b.IntV(42)}),
      Make(Op::kStructGet, {b.Id("sx"), b.Id("s"), b.Id("field")}),
      Make(Op::kReturn, {b.Id("sx")}),
  };
  check("fallout struct create/set/get -> 42",
        ExecuteFunction(b.pex, b.obj, sf, ObjectRef{1}, {}, vm).ToInt() == 42);

  // Fallout 'is' opcode (TestVm answers true for any object).
  Function isf;
  isf.return_type = b.S("Bool");
  isf.params.push_back({b.S("o"), b.S("Form")});
  isf.locals.push_back({b.S("r"), b.S("Bool")});
  isf.code = {
      Make(Op::kIs, {b.Id("r"), b.Id("o"), b.Id("Actor")}),
      Make(Op::kReturn, {b.Id("r")}),
  };
  check("fallout is-opcode executes -> true",
        ExecuteFunction(b.pex, b.obj, isf, ObjectRef{1}, {Value::Object({2})}, vm).ToBool());

  // Event dispatch: TryCall fires a handler only when the instance defines it,
  // and is a silent no-op otherwise (how the engine broadcasts form events like
  // OnDeath / OnItemAdded without warning about forms that ignore them).
  {
    NativeRegistry ev_natives;
    VirtualMachine ev_vm(&ev_natives);
    Builder lb;
    lb.obj.name = lb.S("Listener");
    lb.obj.parent_class = lb.S("");
    MemberVariable hits;
    hits.name = lb.S("::Hits_var");
    hits.type = lb.S("Int");
    hits.initial_value = lb.IntV(0);
    lb.obj.variables.push_back(hits);
    Function on_signal;
    on_signal.return_type = lb.S("");
    on_signal.code = {
        Make(Op::kIAdd, {lb.Id("::Hits_var"), lb.Id("::Hits_var"), lb.IntV(1)}),
        Make(Op::kReturn, {}),
    };
    State def;
    def.name = lb.S("");
    def.functions.push_back({lb.S("OnSignal"), std::move(on_signal)});
    lb.obj.states.push_back(std::move(def));
    lb.pex.objects.push_back(lb.obj);  // MakeScript is defined later in this file
    ev_vm.AddScript(std::move(lb.pex));
    ObjectRef listener = ev_vm.CreateInstance("Listener");
    auto hits_of = [&] {
      Value* m = ev_vm.MemberVar(listener, "::Hits_var");
      return m ? m->ToInt() : -1;
    };
    check("TryCall defined handler dispatches", ev_vm.TryCall(listener, "OnSignal", {}));
    check("handler ran (hits==1)", hits_of() == 1);
    ev_vm.TryCall(listener, "OnSignal", {});
    check("repeat dispatch (hits==2)", hits_of() == 2);
    check("TryCall undefined handler -> false", !ev_vm.TryCall(listener, "OnAbsent", {}));
    check("undefined handler did not run", hits_of() == 2);
    check("TryCall on missing instance -> false", !ev_vm.TryCall(ObjectRef{0xDEAD}, "OnSignal", {}));
  }

  // Multiple scripts on one form (handle): a quest carries its main script plus
  // the Creation-Kit QF_ stage-fragment script. Both must be live and any of
  // their functions dispatchable, regardless of attach order, the stage
  // fragments that display objectives and chain quests live on QF_, which is
  // often not the first-listed script.
  {
    NativeRegistry ms_natives;
    VirtualMachine ms_vm(&ms_natives);
    // Main script: defines OnInit only (stands in for the quest's own script).
    Builder mainb;
    mainb.obj.name = mainb.S("CWMainScript");
    mainb.obj.parent_class = mainb.S("Quest");
    {
      Function on_init;
      on_init.return_type = mainb.S("");
      on_init.code = {Make(Op::kReturn, {})};
      State def;
      def.name = mainb.S("");
      def.functions.push_back({mainb.S("OnInit"), std::move(on_init)});
      mainb.obj.states.push_back(std::move(def));
    }
    mainb.pex.objects.push_back(mainb.obj);
    ms_vm.AddScript(std::move(mainb.pex));
    // QF_ fragment script: a Fragment_3 that returns a sentinel int.
    Builder qfb;
    qfb.obj.name = qfb.S("QF_CWTest");
    qfb.obj.parent_class = qfb.S("Quest");
    {
      Function frag;
      frag.return_type = qfb.S("Int");
      frag.locals.push_back({qfb.S("r"), qfb.S("Int")});
      frag.code = {Make(Op::kAssign, {qfb.Id("r"), qfb.IntV(77)}), Make(Op::kReturn, {qfb.Id("r")})};
      State def;
      def.name = qfb.S("");
      def.functions.push_back({qfb.S("Fragment_3"), std::move(frag)});
      qfb.obj.states.push_back(std::move(def));
    }
    qfb.pex.objects.push_back(qfb.obj);
    ms_vm.AddScript(std::move(qfb.pex));

    const unsigned long long kHandle = 0x3372b;
    ObjectRef a = ms_vm.CreateInstanceWithHandle("CWMainScript", kHandle);
    ObjectRef b2 = ms_vm.CreateInstanceWithHandle("QF_CWTest", kHandle);  // second attach
    ObjectRef dup = ms_vm.CreateInstanceWithHandle("QF_CWTest", kHandle);  // duplicate
    check("first script attaches", a.handle == kHandle);
    check("second script attaches to same handle", b2.handle == kHandle);
    check("duplicate attach is a no-op (returns None)", dup.handle == 0);
    check("primary type is the first-attached script",
          ms_vm.TypeOf(ObjectRef{kHandle}) == "CWMainScript");
    check("fragment on the non-primary script dispatches",
          ms_vm.Call(ObjectRef{kHandle}, "Fragment_3", {}).ToInt() == 77);
    check("is-of-type spans every attached script",
          ms_vm.IsObjectOfType(ObjectRef{kHandle}, "QF_CWTest") &&
              ms_vm.IsObjectOfType(ObjectRef{kHandle}, "CWMainScript") &&
              ms_vm.IsObjectOfType(ObjectRef{kHandle}, "Quest"));
  }

  // End-to-end form event: the Skyrim bindings' AddItem must fire OnItemAdded on
  // the container's script instance, delivering the item count. This is the path
  // a real "acquire item" objective relies on.
  {
    NativeRegistry it_natives;
    VirtualMachine it_vm(&it_natives);
    Builder ob2;
    ob2.obj.name = ob2.S("ObjectReference");
    ob2.obj.parent_class = ob2.S("");
    MemberVariable got;
    got.name = ob2.S("::Got_var");
    got.type = ob2.S("Int");
    got.initial_value = ob2.IntV(0);
    ob2.obj.variables.push_back(got);
    Function on_added;
    on_added.return_type = ob2.S("");
    on_added.params.push_back({ob2.S("akBaseItem"), ob2.S("Form")});
    on_added.params.push_back({ob2.S("aiItemCount"), ob2.S("Int")});
    on_added.params.push_back({ob2.S("akItemReference"), ob2.S("ObjectReference")});
    on_added.params.push_back({ob2.S("akSourceContainer"), ob2.S("ObjectReference")});
    on_added.code = {
        Make(Op::kAssign, {ob2.Id("::Got_var"), ob2.Id("aiItemCount")}),
        Make(Op::kReturn, {}),
    };
    State def2;
    def2.name = ob2.S("");
    def2.functions.push_back({ob2.S("OnItemAdded"), std::move(on_added)});
    ob2.obj.states.push_back(std::move(def2));
    ob2.pex.objects.push_back(ob2.obj);
    it_vm.AddScript(std::move(ob2.pex));
    ObjectRef container = it_vm.CreateInstance("ObjectReference");

    rec::script::skyrim::RecordBackedSkyrimBindings bindings;
    bindings.set_vm(&it_vm);
    bindings.AddItem(container, ObjectRef{0xA11CE}, 5);
    Value* got_m = it_vm.MemberVar(container, "::Got_var");
    check("AddItem fires OnItemAdded (count delivered)", got_m && got_m->ToInt() == 5);
  }

  // End-to-end combat event: ApplyMeleeHit must fire OnHit on the victim's script
  // instance, delivering the aggressor. A "kill N with the player's help" or
  // trap-damage objective relies on this handler running.
  {
    NativeRegistry hit_natives;
    VirtualMachine hit_vm(&hit_natives);
    Builder hb;
    hb.obj.name = hb.S("ObjectReference");
    hb.obj.parent_class = hb.S("");
    MemberVariable hitter;
    hitter.name = hb.S("::Hitter_var");
    hitter.type = hb.S("ObjectReference");
    hitter.initial_value = hb.IntV(0);
    hb.obj.variables.push_back(hitter);
    Function on_hit;
    on_hit.return_type = hb.S("");
    on_hit.params.push_back({hb.S("akAggressor"), hb.S("ObjectReference")});
    on_hit.params.push_back({hb.S("akSource"), hb.S("Form")});
    on_hit.params.push_back({hb.S("akProjectile"), hb.S("Projectile")});
    on_hit.params.push_back({hb.S("abPowerAttack"), hb.S("Bool")});
    on_hit.params.push_back({hb.S("abSneakAttack"), hb.S("Bool")});
    on_hit.params.push_back({hb.S("abBashAttack"), hb.S("Bool")});
    on_hit.params.push_back({hb.S("abHitBlocked"), hb.S("Bool")});
    on_hit.code = {
        Make(Op::kAssign, {hb.Id("::Hitter_var"), hb.Id("akAggressor")}),
        Make(Op::kReturn, {}),
    };
    State hdef;
    hdef.name = hb.S("");
    hdef.functions.push_back({hb.S("OnHit"), std::move(on_hit)});
    hb.obj.states.push_back(std::move(hdef));
    hb.pex.objects.push_back(hb.obj);
    hit_vm.AddScript(std::move(hb.pex));
    ObjectRef victim = hit_vm.CreateInstance("ObjectReference");

    rec::script::skyrim::RecordBackedSkyrimBindings bindings;
    bindings.set_vm(&hit_vm);
    bindings.SetActorValue(victim, "health", 100.0f);
    const ObjectRef aggressor{0xA66E5};
    bindings.ApplyMeleeHit(aggressor, victim, 10.0f);
    Value* hit_m = hit_vm.MemberVar(victim, "::Hitter_var");
    check("ApplyMeleeHit fires OnHit (aggressor delivered)",
          hit_m && hit_m->as_object().handle == aggressor.handle);
  }

  // End-to-end combat state event: StartCombat/StopCombat must fire
  // OnCombatStateChanged on the actor, delivering the state int (1 entering, 0
  // leaving). "aggro triggers an alarm" / "peace ends the scene" logic hangs off
  // this transition.
  {
    NativeRegistry cs_natives;
    VirtualMachine cs_vm(&cs_natives);
    Builder cb;
    cb.obj.name = cb.S("Actor");
    cb.obj.parent_class = cb.S("");
    MemberVariable state;
    state.name = cb.S("::State_var");
    state.type = cb.S("Int");
    state.initial_value = cb.IntV(-1);
    cb.obj.variables.push_back(state);
    MemberVariable calls;
    calls.name = cb.S("::Calls_var");
    calls.type = cb.S("Int");
    calls.initial_value = cb.IntV(0);
    cb.obj.variables.push_back(calls);
    Function on_combat;
    on_combat.return_type = cb.S("");
    on_combat.params.push_back({cb.S("akTarget"), cb.S("Actor")});
    on_combat.params.push_back({cb.S("aeCombatState"), cb.S("Int")});
    on_combat.code = {
        Make(Op::kAssign, {cb.Id("::State_var"), cb.Id("aeCombatState")}),
        Make(Op::kIAdd, {cb.Id("::Calls_var"), cb.Id("::Calls_var"), cb.IntV(1)}),
        Make(Op::kReturn, {}),
    };
    State cdef;
    cdef.name = cb.S("");
    cdef.functions.push_back({cb.S("OnCombatStateChanged"), std::move(on_combat)});
    cb.obj.states.push_back(std::move(cdef));
    cb.pex.objects.push_back(cb.obj);
    cs_vm.AddScript(std::move(cb.pex));
    ObjectRef fighter = cs_vm.CreateInstance("Actor");

    rec::script::skyrim::RecordBackedSkyrimBindings bindings;
    bindings.set_vm(&cs_vm);
    bindings.SetActorValue(fighter, "health", 100.0f);
    const ObjectRef foe{0xF0E01};
    bindings.SetActorValue(foe, "health", 100.0f);

    bindings.StartCombat(fighter, foe);
    Value* st = cs_vm.MemberVar(fighter, "::State_var");
    check("StartCombat fires OnCombatStateChanged (in combat = 1)", st && st->ToInt() == 1);

    // Re-targeting mid-combat is quiet: no second entering-combat call.
    bindings.StartCombat(fighter, ObjectRef{0xF0E02});
    Value* cl = cs_vm.MemberVar(fighter, "::Calls_var");
    check("re-target mid-combat does not re-fire", cl && cl->ToInt() == 1);

    bindings.StopCombat(fighter);
    st = cs_vm.MemberVar(fighter, "::State_var");
    check("StopCombat fires OnCombatStateChanged (out of combat = 0)", st && st->ToInt() == 0);

    // A redundant StopCombat is silent (call count stays at 2).
    bindings.StopCombat(fighter);
    cl = cs_vm.MemberVar(fighter, "::Calls_var");
    check("redundant StopCombat does not re-fire", cl && cl->ToInt() == 2);

    // A target dying pulls its attacker out of combat with an OnCombatStateChanged(0),
    // not a silent erase: the fighter re-engages a foe, the foe dies, and the
    // fighter's handler must see the leaving-combat transition.
    bindings.SetActorValue(fighter, "health", 100.0f);
    const ObjectRef doomed{0xF0E03};
    bindings.SetActorValue(doomed, "health", 10.0f);
    bindings.StartCombat(fighter, doomed);  // call 3: state 1
    bindings.ApplyMeleeHit(fighter, doomed, 999.0f);  // doomed dies -> fighter disengages
    check("target death disengages the attacker", !bindings.IsInCombat(fighter));
    st = cs_vm.MemberVar(fighter, "::State_var");
    cl = cs_vm.MemberVar(fighter, "::Calls_var");
    check("target death fires OnCombatStateChanged(0) on the survivor",
          st && st->ToInt() == 0 && cl && cl->ToInt() == 4);
  }

  // End-to-end equip events: EquipItem/UnequipItem flip IsEquipped and fire
  // OnObjectEquipped/OnObjectUnequipped on the actor plus OnEquipped/OnUnequipped
  // on the item. A "wear the guard armor" objective relies on these.
  {
    NativeRegistry eq_natives;
    VirtualMachine eq_vm(&eq_natives);
    // Actor script: records the equipped base form and counts equip/unequip fires.
    Builder ab;
    ab.obj.name = ab.S("EquipActor");
    ab.obj.parent_class = ab.S("");
    auto int_var = [](Builder& b, const char* name) {
      MemberVariable v;
      v.name = b.S(name);
      v.type = b.S("Int");
      v.initial_value = b.IntV(0);
      return v;
    };
    MemberVariable worn;
    worn.name = ab.S("::Worn_var");
    worn.type = ab.S("Form");
    worn.initial_value = ab.IntV(0);
    ab.obj.variables.push_back(worn);
    ab.obj.variables.push_back(int_var(ab, "::EqCount_var"));
    ab.obj.variables.push_back(int_var(ab, "::UneqCount_var"));
    Function on_eq;
    on_eq.return_type = ab.S("");
    on_eq.params.push_back({ab.S("akBaseObject"), ab.S("Form")});
    on_eq.params.push_back({ab.S("akReference"), ab.S("ObjectReference")});
    on_eq.code = {
        Make(Op::kAssign, {ab.Id("::Worn_var"), ab.Id("akBaseObject")}),
        Make(Op::kIAdd, {ab.Id("::EqCount_var"), ab.Id("::EqCount_var"), ab.IntV(1)}),
        Make(Op::kReturn, {}),
    };
    Function on_uneq;
    on_uneq.return_type = ab.S("");
    on_uneq.params.push_back({ab.S("akBaseObject"), ab.S("Form")});
    on_uneq.params.push_back({ab.S("akReference"), ab.S("ObjectReference")});
    on_uneq.code = {
        Make(Op::kIAdd, {ab.Id("::UneqCount_var"), ab.Id("::UneqCount_var"), ab.IntV(1)}),
        Make(Op::kReturn, {}),
    };
    State adef;
    adef.name = ab.S("");
    adef.functions.push_back({ab.S("OnObjectEquipped"), std::move(on_eq)});
    adef.functions.push_back({ab.S("OnObjectUnequipped"), std::move(on_uneq)});
    ab.obj.states.push_back(std::move(adef));
    ab.pex.objects.push_back(ab.obj);
    eq_vm.AddScript(std::move(ab.pex));
    ObjectRef actor = eq_vm.CreateInstance("EquipActor");

    // Item script: flips a flag on OnEquipped/OnUnequipped.
    Builder ib;
    ib.obj.name = ib.S("EquipItemScr");
    ib.obj.parent_class = ib.S("");
    ib.obj.variables.push_back(int_var(ib, "::On_var"));
    Function ion;
    ion.return_type = ib.S("");
    ion.params.push_back({ib.S("akActor"), ib.S("Actor")});
    ion.code = {Make(Op::kAssign, {ib.Id("::On_var"), ib.IntV(1)}), Make(Op::kReturn, {})};
    Function ioff;
    ioff.return_type = ib.S("");
    ioff.params.push_back({ib.S("akActor"), ib.S("Actor")});
    ioff.code = {Make(Op::kAssign, {ib.Id("::On_var"), ib.IntV(0)}), Make(Op::kReturn, {})};
    State idef;
    idef.name = ib.S("");
    idef.functions.push_back({ib.S("OnEquipped"), std::move(ion)});
    idef.functions.push_back({ib.S("OnUnequipped"), std::move(ioff)});
    ib.obj.states.push_back(std::move(idef));
    ib.pex.objects.push_back(ib.obj);
    eq_vm.AddScript(std::move(ib.pex));
    ObjectRef item = eq_vm.CreateInstance("EquipItemScr");

    rec::script::skyrim::RecordBackedSkyrimBindings bindings;
    bindings.set_vm(&eq_vm);

    bindings.EquipItem(actor, item);
    Value* worn_m = eq_vm.MemberVar(actor, "::Worn_var");
    Value* eqc = eq_vm.MemberVar(actor, "::EqCount_var");
    Value* on_m = eq_vm.MemberVar(item, "::On_var");
    check("EquipItem fires OnObjectEquipped (item delivered)",
          worn_m && worn_m->as_object().handle == item.handle && eqc && eqc->ToInt() == 1);
    check("EquipItem fires OnEquipped on the item", on_m && on_m->ToInt() == 1);
    check("IsEquipped true after equip", bindings.IsEquipped(actor, item));

    bindings.EquipItem(actor, item);  // already equipped: no second fire
    eqc = eq_vm.MemberVar(actor, "::EqCount_var");
    check("re-equip does not re-fire", eqc && eqc->ToInt() == 1);

    bindings.UnequipItem(actor, item);
    Value* uneqc = eq_vm.MemberVar(actor, "::UneqCount_var");
    on_m = eq_vm.MemberVar(item, "::On_var");
    check("UnequipItem fires OnObjectUnequipped", uneqc && uneqc->ToInt() == 1);
    check("UnequipItem fires OnUnequipped on the item", on_m && on_m->ToInt() == 0);
    check("IsEquipped false after unequip", !bindings.IsEquipped(actor, item));

    bindings.UnequipItem(actor, item);  // not equipped: no second fire
    uneqc = eq_vm.MemberVar(actor, "::UneqCount_var");
    check("re-unequip does not re-fire", uneqc && uneqc->ToInt() == 1);
  }

  // Alias death dispatch: an actor that fills a quest alias should deliver its
  // death to the alias's OnDeath script. This is the Civil War reinforcement
  // path, a soldier dies and its CWReinforcementAliasScript.OnDeath runs.
  {
    NativeRegistry al_natives;
    VirtualMachine al_vm(&al_natives);
    Builder ab;
    ab.obj.name = ab.S("CWReinforcementAliasScript");
    ab.obj.parent_class = ab.S("");
    MemberVariable deaths;
    deaths.name = ab.S("::Deaths_var");
    deaths.type = ab.S("Int");
    deaths.initial_value = ab.IntV(0);
    ab.obj.variables.push_back(deaths);
    Function on_death;
    on_death.return_type = ab.S("");
    on_death.params.push_back({ab.S("akKiller"), ab.S("Actor")});
    on_death.code = {
        Make(Op::kIAdd, {ab.Id("::Deaths_var"), ab.Id("::Deaths_var"), ab.IntV(1)}),
        Make(Op::kReturn, {}),
    };
    State adef;
    adef.name = ab.S("");
    adef.functions.push_back({ab.S("OnDeath"), std::move(on_death)});
    ab.obj.states.push_back(std::move(adef));
    ab.pex.objects.push_back(ab.obj);
    al_vm.AddScript(std::move(ab.pex));

    const unsigned long long alias_handle =
        rec::script::papyrus::EncodeAliasHandle(/*quest=*/0x83042, /*alias_id=*/1);
    al_vm.CreateInstanceWithHandle("CWReinforcementAliasScript", alias_handle);

    rec::script::skyrim::RecordBackedSkyrimBindings binds;
    binds.set_vm(&al_vm);
    const ObjectRef soldier{0x500};
    binds.SetActorValue(soldier, "health", 50.0f);
    binds.AliasForceRefTo(ObjectRef{alias_handle}, soldier);
    binds.SetActorValue(soldier, "health", 0.0f);  // the soldier dies
    Value* d = al_vm.MemberVar(ObjectRef{alias_handle}, "::Deaths_var");
    check("filled actor death runs its alias OnDeath", d && d->ToInt() == 1);
    // Clearing the fill drops the link: a later death must not re-fire it.
    binds.AliasClear(ObjectRef{alias_handle});
    binds.SetActorValue(soldier, "health", 100.0f);  // revive (re-arms the death)
    binds.SetActorValue(soldier, "health", 0.0f);    // dies again, but unaliased now
    check("cleared alias no longer receives the death", d && d->ToInt() == 1);
  }

  // Bare-ref native dispatch: a method call on a ref with no script instance
  // (most refs, and quest aliases) still reaches an engine native registered on
  // a base type, instead of silently returning None. None (handle 0) does not.
  {
    NativeRegistry bare_natives;
    bare_natives.Register("ObjectReference", "Is3DLoaded",
                          [](VirtualMachine&, ObjectRef, std::vector<Value>&) {
                            return Value::Bool(true);
                          });
    bare_natives.Register("ReferenceAlias", "GetReference",
                          [](VirtualMachine&, ObjectRef self, std::vector<Value>&) {
                            return Value::Object(ObjectRef{self.handle + 1});
                          });
    VirtualMachine bare_vm(&bare_natives);
    const ObjectRef bare{0x1234};
    check("bare ref dispatches an ObjectReference native",
          bare_vm.Call(bare, "Is3DLoaded", {}).ToBool());
    check("bare ref dispatches a ReferenceAlias native",
          bare_vm.Call(bare, "GetReference", {}).as_object().handle == 0x1235);
    check("None (handle 0) does not dispatch",
          bare_vm.Call(ObjectRef{0}, "Is3DLoaded", {}).ToBool() == false);
    check("an unbound bare-ref method is None",
          bare_vm.Call(bare, "NoSuchMethod", {}).type() == rec::script::papyrus::ValueType::kNone);
  }

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

const char* OpMnemonic(Op op) {
  switch (op) {
    case Op::kNop: return "nop";
    case Op::kIAdd: return "iadd";
    case Op::kFAdd: return "fadd";
    case Op::kISub: return "isub";
    case Op::kFSub: return "fsub";
    case Op::kIMul: return "imul";
    case Op::kFMul: return "fmul";
    case Op::kIDiv: return "idiv";
    case Op::kFDiv: return "fdiv";
    case Op::kIMod: return "imod";
    case Op::kNot: return "not";
    case Op::kINeg: return "ineg";
    case Op::kFNeg: return "fneg";
    case Op::kAssign: return "assign";
    case Op::kCast: return "cast";
    case Op::kCmpEq: return "cmp_eq";
    case Op::kCmpLt: return "cmp_lt";
    case Op::kCmpLe: return "cmp_le";
    case Op::kCmpGt: return "cmp_gt";
    case Op::kCmpGe: return "cmp_ge";
    case Op::kJmp: return "jmp";
    case Op::kJmpT: return "jmpt";
    case Op::kJmpF: return "jmpf";
    case Op::kCallMethod: return "callmethod";
    case Op::kCallParent: return "callparent";
    case Op::kCallStatic: return "callstatic";
    case Op::kReturn: return "return";
    case Op::kStrCat: return "strcat";
    case Op::kPropGet: return "propget";
    case Op::kPropSet: return "propset";
    case Op::kArrayCreate: return "array_create";
    case Op::kArrayLength: return "array_length";
    case Op::kArrayGetElement: return "array_get";
    case Op::kArraySetElement: return "array_set";
    case Op::kArrayFindElement: return "array_find";
    case Op::kArrayRFindElement: return "array_rfind";
    default: return "op";
  }
}

std::string FmtArg(const PexFile& pex, const VariableData& v) {
  switch (v.type) {
    case VariableData::Type::kIdentifier: return pex.Str(v.string_index);
    case VariableData::Type::kString: return "\"" + pex.Str(v.string_index) + "\"";
    case VariableData::Type::kInteger: return std::to_string(v.int_value);
    case VariableData::Type::kFloat: return std::to_string(v.float_value);
    case VariableData::Type::kBool: return v.bool_value ? "true" : "false";
    default: return "None";
  }
}

// Prints one non-native function's bytecode so a runaway loop (a backward jump)
// and what it polls (the call targets inside it) can be read off directly.
int DisasmReal(const std::string& data_dir, const std::string& script, const std::string& function) {
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
  // "@funcs" lists every function the script declares, with its states.
  if (function == "@funcs") {
    std::printf("%s : %s\n", pex.Str(obj.name).c_str(), pex.Str(obj.parent_class).c_str());
    for (const State& s : obj.states)
      for (const NamedFunction& nf : s.functions)
        std::printf("  %s%s%s\n", pex.Str(nf.name).c_str(),
                    nf.function.is_native ? " (native)" : "",
                    s.name == obj.auto_state_name ? "" : (" [state " + pex.Str(s.name) + "]").c_str());
    return 0;
  }
  // "@vars" dumps the object header (member variables and properties with their
  // types) instead of a function, to inspect e.g. an alias property's type.
  if (function == "@vars") {
    std::printf("%s : %s\n", pex.Str(obj.name).c_str(), pex.Str(obj.parent_class).c_str());
    for (const MemberVariable& v : obj.variables)
      std::printf("  var %s : %s\n", pex.Str(v.name).c_str(), pex.Str(v.type).c_str());
    for (const Property& p : obj.properties)
      std::printf("  property %s : %s\n", pex.Str(p.name).c_str(), pex.Str(p.type).c_str());
    return 0;
  }
  const Object* owner = nullptr;
  const Function* fn = FindFunction(pex, obj, function, &owner);
  if (!fn) {
    std::printf("no function %s in %s\n", function.c_str(), script.c_str());
    return 1;
  }
  std::printf("%s.%s  (%zu instructions)\n", script.c_str(), function.c_str(), fn->code.size());
  for (size_t i = 0; i < fn->code.size(); ++i) {
    const Instruction& in = fn->code[i];
    std::string line = "  " + std::to_string(i) + ": " + OpMnemonic(in.op);
    for (const VariableData& a : in.args) line += " " + FmtArg(pex, a);
    if (!in.var_args.empty()) {
      line += " [";
      for (size_t j = 0; j < in.var_args.size(); ++j)
        line += (j ? ", " : "") + FmtArg(pex, in.var_args[j]);
      line += "]";
    }
    std::printf("%s\n", line.c_str());
  }
  return 0;
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

  // Alias event routing: with an alias resolver set, RaiseEvent reaches both the
  // target ref's script and the scripts on the aliases it fills, the path an
  // engine-raised OnActivate/OnTriggerEnter takes to a quest/alias listener.
  {
    auto ping_script = [](const char* type_name) {
      Builder b;
      b.obj.name = b.S(type_name);
      b.obj.parent_class = b.S("");
      MemberVariable pinged;
      pinged.name = b.S("::Pinged_var");
      pinged.type = b.S("Int");
      pinged.initial_value = b.IntV(0);
      b.obj.variables.push_back(pinged);
      Function on_ping;
      on_ping.return_type = b.S("");
      on_ping.code = {
          Make(Op::kIAdd, {b.Id("::Pinged_var"), b.Id("::Pinged_var"), b.IntV(1)}),
          Make(Op::kReturn, {}),
      };
      State def;
      def.name = b.S("");
      def.functions.push_back({b.S("OnPing"), std::move(on_ping)});
      b.obj.states.push_back(std::move(def));
      b.pex.objects.push_back(b.obj);
      return std::move(b.pex);
    };
    guest.SubmitFor([f = ping_script("PingRef")](VirtualMachine& vm) mutable {
           return vm.AddScript(std::move(f));
         }).get();
    guest.SubmitFor([f = ping_script("PingAlias")](VirtualMachine& vm) mutable {
           return vm.AddScript(std::move(f));
         }).get();

    const ObjectRef ref{0x9001};
    const unsigned long long alias_handle =
        rec::script::papyrus::EncodeAliasHandle(/*quest=*/0x9002, /*alias_id=*/2);
    guest.SubmitFor([ref](VirtualMachine& vm) {
           return vm.CreateInstanceWithHandle("PingRef", ref.handle);
         }).get();
    guest.SubmitFor([alias_handle](VirtualMachine& vm) {
           return vm.CreateInstanceWithHandle("PingAlias", alias_handle);
         }).get();
    // Set the resolver on the guest thread (its only safe writer), mapping the ref
    // to the one alias it fills.
    guest.SubmitFor([&guest, ref, alias_handle](VirtualMachine&) {
           guest.set_alias_resolver([ref, alias_handle](ObjectRef r) {
             std::vector<ObjectRef> out;
             if (r.handle == ref.handle) out.push_back(ObjectRef{alias_handle});
             return out;
           });
           return 0;
         }).get();

    guest.RaiseEvent(ref, "OnPing");
    guest.SubmitFor([](VirtualMachine&) { return 0; }).get();  // flush the async RaiseEvent
    auto pinged = [&](ObjectRef o) {
      return guest
          .SubmitFor([o](VirtualMachine& vm) {
            Value* m = vm.MemberVar(o, "::Pinged_var");
            return m ? m->ToInt() : -1;
          })
          .get();
    };
    check("RaiseEvent reaches the target ref", pinged(ref) == 1);
    check("RaiseEvent routes to the filled alias", pinged(ObjectRef{alias_handle}) == 1);
  }

  guest.Stop();
  std::printf("%s (%d failures)\n", failures ? "GUESTTEST FAILED" : "GUESTTEST PASSED", failures);
  return failures ? 1 : 0;
}

// Engine bindings stub with known values, so the managed SDK self-test can
// assert on dispatched native results.
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

// Drives the full two-worlds path: boots the .NET CoreCLR host, hands it a
// bridge to a live Papyrus guest backed by known bindings, and lets the managed
// SDK self-test dispatch natives (Game.Player and its actor-value/state calls)
// and run an instance lifecycle (create, call, tick, read a property). The
// managed side returns its failure count; this asserts it is zero. Skips
// cleanly (rc 0) when no .NET runtime is present.
int HostTest(const std::string& runtime_config, const std::string& assembly) {
  using rec::script::PapyrusGuest;

  TestBindings bindings;
  TickerScripts scripts = BuildTickerScripts();
  PapyrusGuest guest(bethesda::Game::kSkyrimSe);
  rec::script::skyrim::RegisterSkyrimNatives(guest.natives(), &bindings);
  guest.Start();
  guest.SubmitFor([f = std::move(scripts.form)](VirtualMachine& vm) mutable {
        return vm.AddScript(std::move(f));
      }).get();
  guest.SubmitFor([t = std::move(scripts.ticker)](VirtualMachine& vm) mutable {
        return vm.AddScript(std::move(t));
      }).get();

  rec::script::host::BridgeContext bridge_ctx{&guest, {}};
  rec::script::host::ScriptBridge bridge = rec::script::host::MakeScriptBridge(bridge_ctx);
  rec::script::host::ClrHost host;
  if (!host.Initialize(/*dotnet_root=*/"", runtime_config, assembly,
                       "Recreation.ScriptHost, Recreation.Scripting", "SelfTest")) {
    std::printf("HOSTTEST SKIPPED (no .NET runtime / entrypoint)\n");
    guest.Stop();
    return 0;
  }

  int failures = host.Invoke(&bridge);
  std::printf("[native] managed SelfTest reported %d failure(s)\n", failures);

  host.Shutdown();
  guest.Stop();

  bool ok = failures == 0;
  std::printf("%s\n", ok ? "HOSTTEST PASSED" : "HOSTTEST FAILED");
  return ok ? 0 : 1;
}

// Drives the engine->managed direction end to end through the ManagedHost: boots
// the production managed world (which loads the Skyrim mod and its attribute
// regeneration), ticks it, and confirms the player's health rose back toward its
// base. That round trip proves the full path: engine tick -> managed OnUpdate ->
// SDK call back into the engine -> bindings state changes -> engine observes.
// Skips cleanly (rc 0) when no .NET runtime is present.
int ManagedHostTest(const std::string& runtime_config, const std::string& assembly) {
  using rec::script::PapyrusGuest;
  using rec::script::skyrim::RecordBackedSkyrimBindings;

  RecordBackedSkyrimBindings bindings;
  const ObjectRef player{0x14};
  bindings.set_player(player);
  // Player at half health: base 100, current 50.
  bindings.SetActorValue(player, "Health", 100.0f);
  bindings.ModActorValue(player, "Health", -50.0f);

  PapyrusGuest guest(bethesda::Game::kSkyrimSe);
  rec::script::skyrim::RegisterSkyrimNatives(guest.natives(), &bindings);
  guest.Start();

  rec::script::host::ManagedHost host;
  host.AddDomain("Skyrim Special Edition", guest, /*loader=*/{});
  if (!host.Boot(/*dotnet_root=*/"", runtime_config, assembly)) {
    std::printf("MANAGEDHOSTTEST SKIPPED (no .NET runtime / entrypoint)\n");
    guest.Stop();
    return 0;
  }

  const f32 before = bindings.GetActorValue(player, "Health");
  for (int i = 0; i < 10; ++i) host.Tick(1.0f);  // ~0.7 hp/s * 10s
  const f32 after = bindings.GetActorValue(player, "Health");

  // Event delivery must not fault even without a consumer.
  host.PublishEvent({rec::script::host::ManagedEventId::kActorDied, player.handle, 0, 0, 0.0f});

  host.Shutdown();
  guest.Stop();

  int failures = 0;
  auto check = [&](const char* what, bool ok) {
    std::printf("  %-44s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
  };
  check("health regenerated over ticks", after > before);
  check("regen stays within base", after <= 100.0f);
  check("regen advanced roughly 7 hp", after > 56.0f && after < 58.0f);
  std::printf("%s\n", failures ? "MANAGEDHOSTTEST FAILED" : "MANAGEDHOSTTEST PASSED");
  return failures ? 1 : 0;
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

  // ReferenceAlias.GetOwningQuest decodes the quest from the alias handle (the
  // bare-ref native path, no script instance), so an alias's OnDeath can
  // GetOwningQuest().registerDeath(self).
  const unsigned long long alias = rec::script::papyrus::EncodeAliasHandle(0x3372b, 7);
  check("ReferenceAlias.GetOwningQuest decodes its quest",
        vm.Call(ObjectRef{alias}, "GetOwningQuest", {}).as_object().handle == 0x3372b);

  std::printf("%s (%d failures)\n", failures ? "SKYRIMTEST FAILED" : "SKYRIMTEST PASSED", failures);
  return failures ? 1 : 0;
}

// Hammers the guest's thread-safe facade from many host threads at once. The
// guest runs the VM on one dedicated thread, so concurrent submissions must
// serialize there with no lost work, no race and no deadlock. The direct test
// of "the papyrus vm runs compartmentalized on its own thread".
int ConcurrencyTest() {
  using rec::script::PapyrusGuest;
  Builder cb;
  cb.obj.name = cb.S("Counter");
  cb.obj.parent_class = cb.S("");
  {
    MemberVariable n;
    n.name = cb.S("n");
    n.type = cb.S("Int");
    n.initial_value = cb.IntV(0);
    cb.obj.variables.push_back(n);
    State def;
    def.name = cb.S("");
    cb.obj.states.push_back(std::move(def));
  }
  PexFile counter = MakeScript(cb);

  PapyrusGuest guest(bethesda::Game::kSkyrimSe);
  guest.Start();
  guest.SubmitFor([c = std::move(counter)](VirtualMachine& vm) mutable {
        return vm.AddScript(std::move(c));
      }).get();
  ObjectRef shared = guest.CreateInstance("Counter").get();

  const int kThreads = 8, kPerThread = 1500;
  std::atomic<int> bad_handles{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < kPerThread; ++i) {
        guest
            .SubmitFor([shared](VirtualMachine& vm) {
              Value* m = vm.MemberVar(shared, "n");
              if (m) *m = Value::Int(m->ToInt() + 1);
              return 0;
            })
            .get();
      }
      if (guest.CreateInstance("Counter").get().handle == 0) ++bad_handles;
    });
  }
  for (std::thread& th : threads) th.join();
  int final = guest
                  .SubmitFor([shared](VirtualMachine& vm) {
                    Value* m = vm.MemberVar(shared, "n");
                    return m ? m->ToInt() : -1;
                  })
                  .get();
  guest.Stop();

  int failures = 0;
  auto check = [&](const char* what, bool ok) {
    std::printf("  %-44s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
  };
  check("no lost updates under 8-thread contention", final == kThreads * kPerThread);
  check("concurrent CreateInstance all valid", bad_handles.load() == 0);
  std::printf("%s (%d failures)\n", failures ? "CONCTEST FAILED" : "CONCTEST PASSED", failures);
  return failures ? 1 : 0;
}

// Proves the VM core is game-agnostic: a fabricated foreign-game native table
// (standing in for a future Fallout dialect) plugs into the same core and
// dispatches, with no Skyrim surface present. The test of "make it sound and
// separated" so adding FO4/76 never touches the VM core.
int SeparationTest() {
  int failures = 0;
  auto check = [&](const char* what, bool ok) {
    std::printf("  %-44s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
  };

  NativeRegistry foreign;
  foreign.Register("Workshop", "GetObjectCount",
                   [](VirtualMachine&, ObjectRef, std::vector<Value>&) { return Value::Int(76); });
  VirtualMachine vm(&foreign);

  Builder b;
  b.obj.name = b.S("WorkshopUser");
  b.obj.parent_class = b.S("");
  Function f;
  f.return_type = b.S("Int");
  f.is_global = true;
  f.locals.push_back({b.S("r"), b.S("Int")});
  Instruction cs;
  cs.op = Op::kCallStatic;
  cs.args = {b.Id("Workshop"), b.Id("GetObjectCount"), b.Id("r")};
  f.code = {cs, Make(Op::kReturn, {b.Id("r")})};
  State def;
  def.name = b.S("");
  def.functions.push_back({b.S("run"), std::move(f)});
  b.obj.states.push_back(std::move(def));
  std::string type = vm.AddScript(MakeScript(b));

  check("VM core dispatches a foreign-game native", vm.CallGlobal(type, "run", {}).ToInt() == 76);
  check("no skyrim surface in a foreign table", foreign.Find("Math", "Sqrt") == nullptr);
  check("foreign table carries only its own natives", foreign.size() == 1);
  std::printf("%s (%d failures)\n", failures ? "SEPARATIONTEST FAILED" : "SEPARATIONTEST PASSED",
              failures);
  return failures ? 1 : 0;
}

// Verifies the VM's native-call trace ring: off by default, records only while
// enabled, keeps a running total, and clears. This backs the debug trace window.
int TraceTest() {
  NativeRegistry natives;
  rec::script::skyrim::RegisterSkyrimNatives(natives, nullptr);
  VirtualMachine vm(&natives);
  int failures = 0;
  auto check = [&](const char* what, bool ok) {
    std::printf("  %-44s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
  };

  check("tracing off by default", !vm.native_trace());
  vm.set_native_trace(true);
  vm.CallGlobal("Math", "Sqrt", {Value::Float(9)});
  vm.CallGlobal("Math", "Abs", {Value::Float(-1)});
  vm.CallGlobal("Utility", "RandomInt", {Value::Int(1), Value::Int(1)});
  check("3 native calls counted", vm.native_call_count() == 3);
  check("3 calls in the ring", vm.native_trace_log().size() == 3);
  check("oldest is Math.Sqrt", !vm.native_trace_log().empty() &&
                                   vm.native_trace_log().front().function == "Sqrt");
  check("newest is Utility.RandomInt", vm.native_trace_log().back().function == "RandomInt");

  vm.ClearNativeTrace();
  check("clear empties ring + count", vm.native_call_count() == 0 && vm.native_trace_log().empty());

  vm.set_native_trace(false);
  vm.CallGlobal("Math", "Sqrt", {Value::Float(4)});
  check("disabled: counted, not ringed", vm.native_call_count() == 1 && vm.native_trace_log().empty());

  // The exact path the engine debug window drives: enable, call, and snapshot
  // all marshalled through the guest thread.
  using rec::script::PapyrusGuest;
  PapyrusGuest guest(bethesda::Game::kSkyrimSe);
  rec::script::skyrim::RegisterSkyrimNatives(guest.natives(), nullptr);
  guest.Start();
  guest.Submit([](VirtualMachine& g) { g.set_native_trace(true); });
  guest.SubmitFor([](VirtualMachine& g) {
        g.CallGlobal("Math", "Sqrt", {Value::Float(16)});
        g.CallGlobal("Math", "Abs", {Value::Float(-2)});
        return 0;
      }).get();
  auto log = guest.SubmitFor([](VirtualMachine& g) { return g.native_trace_log(); }).get();
  guest.Stop();
  check("guest-thread snapshot has 2 natives", log.size() == 2);
  check("guest-thread newest is Math.Abs", !log.empty() && log.back().function == "Abs");

  std::printf("%s (%d failures)\n", failures ? "TRACETEST FAILED" : "TRACETEST PASSED", failures);
  return failures ? 1 : 0;
}

// Correctness gate for the two boundary tunings: the guest's same-thread
// Dispatch fast path and the NativeBackend arena marshalling. Registers a few
// pure natives (echo a value, add two ints, join string args), then runs the
// managed BridgeCheck battery through the real bridge BOTH from the host thread
// (so each call round-trips to the guest) and from the guest thread (so Dispatch
// hits the VM directly). Both must report zero failures: identical, correct
// results from either thread is what proves the fast path preserves behaviour,
// and the string/growth cases prove the arena marshals correctly. Skips cleanly
// (rc 0) when no .NET runtime is present.
int BridgeTest(const std::string& runtime_config, const std::string& assembly) {
  using rec::script::PapyrusGuest;

  TestBindings bindings;
  PapyrusGuest guest(bethesda::Game::kSkyrimSe);
  rec::script::skyrim::RegisterSkyrimNatives(guest.natives(), &bindings);
  guest.natives().Register(
      "Bench", "Echo", [](VirtualMachine&, ObjectRef, std::vector<Value>& a) {
        return a.empty() ? Value() : a[0];
      });
  guest.natives().Register(
      "Bench", "AddInts", [](VirtualMachine&, ObjectRef, std::vector<Value>& a) {
        return Value::Int((a.size() > 0 ? a[0].ToInt() : 0) + (a.size() > 1 ? a[1].ToInt() : 0));
      });
  guest.natives().Register(
      "Bench", "Join", [](VirtualMachine&, ObjectRef, std::vector<Value>& a) {
        std::string s;
        for (const Value& v : a) s += v.ToString();
        return Value::Str(s);
      });
  guest.Start();

  rec::script::host::BridgeContext bridge_ctx{&guest, {}};
  rec::script::host::ScriptBridge bridge = rec::script::host::MakeScriptBridge(bridge_ctx);
  rec::script::host::ClrHost host;
  if (!host.Initialize(/*dotnet_root=*/"", runtime_config, assembly,
                       "Recreation.ScriptHost, Recreation.Scripting", "BridgeCheck")) {
    std::printf("BRIDGETEST SKIPPED (no .NET runtime / entrypoint)\n");
    guest.Stop();
    return 0;
  }

  const int cross = host.Invoke(&bridge);  // host thread -> guest hop
  const int same =
      guest.SubmitFor([&](VirtualMachine&) { return host.Invoke(&bridge); }).get();  // Dispatch

  host.Shutdown();
  guest.Stop();

  std::printf("  cross-thread bridge checks : %d failure(s)\n", cross);
  std::printf("  same-thread  bridge checks : %d failure(s)\n", same);
  const int failures = cross + same;
  std::printf("%s\n", failures ? "BRIDGETEST FAILED" : "BRIDGETEST PASSED");
  return failures ? 1 : 0;
}

// Measures the C#->engine call throughput through the real bridge, both
// cross-thread (managed loop on the host thread, so every call round-trips to
// the guest via SubmitFor().get()) and same-thread (managed loop run on the
// guest thread, so Dispatch hits the VM directly). Reports ns per bridge call
// and managed bytes allocated per call for each mode. This is what validates
// the two tunings: same-thread vs cross-thread ns isolates the hop cost (#1),
// and bytes/call before vs after the NativeBackend change isolates the
// per-call allocations (#2). Skips cleanly (rc 0) without a .NET runtime.
int BenchBridge(const std::string& runtime_config, const std::string& assembly) {
  using rec::script::PapyrusGuest;

  TestBindings bindings;
  PapyrusGuest guest(bethesda::Game::kSkyrimSe);
  rec::script::skyrim::RegisterSkyrimNatives(guest.natives(), &bindings);
  // A silent global native that echoes its first argument back, so a call can
  // exercise the string-argument marshalling path without any log spam.
  guest.natives().Register(
      "Bench", "Echo", [](VirtualMachine&, ObjectRef, std::vector<Value>& args) {
        return args.empty() ? Value() : args[0];
      });
  guest.Start();

  rec::script::host::BridgeContext bridge_ctx{&guest, {}};
  rec::script::host::ScriptBridge bridge = rec::script::host::MakeScriptBridge(bridge_ctx);
  rec::script::host::ClrHost host;
  if (!host.Initialize(/*dotnet_root=*/"", runtime_config, assembly,
                       "Recreation.ScriptHost, Recreation.Scripting", "Bench")) {
    std::printf("BENCHBRIDGE SKIPPED (no .NET runtime / entrypoint)\n");
    guest.Stop();
    return 0;
  }

  // Mirrors ScriptHost.BenchArgs (sequential layout).
  struct BenchArgs {
    rec::script::host::ScriptBridge* bridge;
    std::int64_t iters;
    std::int64_t alloc_bytes;
  };
  constexpr std::int64_t kIters = 200000;    // loop passes
  constexpr std::int64_t kCallsPerIter = 2;  // bridge calls per pass
  constexpr int kTrials = 5;

  auto run = [&](bool on_guest, double* ns_per_call, double* bytes_per_call) {
    double best_ns = 1e300;
    std::int64_t alloc = 0;
    for (int trial = 0; trial < kTrials; ++trial) {
      BenchArgs a{&bridge, kIters, 0};
      auto t0 = std::chrono::steady_clock::now();
      if (on_guest)
        guest.SubmitFor([&](VirtualMachine&) { return host.Invoke(&a); }).get();
      else
        host.Invoke(&a);
      auto t1 = std::chrono::steady_clock::now();
      double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
      best_ns = std::min(best_ns, ns);
      alloc = a.alloc_bytes;
    }
    const double calls = static_cast<double>(kIters * kCallsPerIter);
    *ns_per_call = best_ns / calls;
    *bytes_per_call = static_cast<double>(alloc) / calls;
  };

  double cross_ns = 0, cross_bytes = 0, same_ns = 0, same_bytes = 0;
  run(/*on_guest=*/false, &cross_ns, &cross_bytes);
  run(/*on_guest=*/true, &same_ns, &same_bytes);

  std::printf("BENCHBRIDGE (%lld iters x %lld calls, best of %d trials)\n",
              static_cast<long long>(kIters), static_cast<long long>(kCallsPerIter), kTrials);
  std::printf("  cross-thread (host thread -> guest hop) : %8.1f ns/call  %6.1f bytes/call\n",
              cross_ns, cross_bytes);
  std::printf("  same-thread  (guest thread, Dispatch)   : %8.1f ns/call  %6.1f bytes/call\n",
              same_ns, same_bytes);
  if (same_ns > 0)
    std::printf("  #1 same-thread speedup                  : %6.2fx (%.1f -> %.1f ns/call)\n",
                cross_ns / same_ns, cross_ns, same_ns);

  host.Shutdown();
  guest.Stop();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "bridgetest") return BridgeTest(argv[2], argv[3]);
  if (argc == 4 && std::string(argv[1]) == "benchbridge") return BenchBridge(argv[2], argv[3]);
  if (argc == 2 && std::string(argv[1]) == "selftest") return SelfTest();
  if (argc == 2 && std::string(argv[1]) == "skyrimtest") return SkyrimTest();
  if (argc == 2 && std::string(argv[1]) == "guesttest") return GuestTest();
  if (argc == 2 && std::string(argv[1]) == "conctest") return ConcurrencyTest();
  if (argc == 2 && std::string(argv[1]) == "separationtest") return SeparationTest();
  if (argc == 2 && std::string(argv[1]) == "tracetest") return TraceTest();
  if (argc == 3 && std::string(argv[1]) == "vmtest") return VmTest(argv[2]);
  if (argc == 4 && std::string(argv[1]) == "hosttest") return HostTest(argv[2], argv[3]);
  if (argc == 4 && std::string(argv[1]) == "managedhosttest")
    return ManagedHostTest(argv[2], argv[3]);
  if (argc == 5 && std::string(argv[1]) == "disasm") return DisasmReal(argv[2], argv[3], argv[4]);
  if (argc == 4) return RunReal(argv[1], argv[2], argv[3]);
  std::fprintf(stderr,
               "usage: %s [selftest|skyrimtest|guesttest|conctest|separationtest]\n"
               "       %s vmtest <data_dir>\n"
               "       %s hosttest <runtimeconfig.json> <assembly.dll>\n"
               "       %s disasm <data_dir> <Script> <Function>\n"
               "       %s <data_dir> <Script> <Function>\n",
               argv[0], argv[0], argv[0], argv[0], argv[0]);
  return 2;
}
