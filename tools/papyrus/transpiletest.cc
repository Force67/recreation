// transpiletest: the experimental Papyrus->C# decompiler, driven off an
// in-memory PexFile so it needs no game archives. Hand-builds two functions
// whose bytecode mirrors what the Papyrus compiler emits, an if/else and a
// while loop with a static call, and asserts the reconstructed C# reads the way
// a modder would expect, exercising temp inlining, control-flow structuring,
// members, and auto properties.

#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "script/papyrus/pex.h"
#include "script/papyrus/transpile.h"

namespace {

using namespace rx::script::papyrus;

// Interns strings into a PexFile's table and hands back stable indices.
struct Builder {
  PexFile pex;
  std::unordered_map<std::string, StringIndex> table;

  StringIndex S(const std::string& s) {
    auto it = table.find(s);
    if (it != table.end()) return it->second;
    auto idx = static_cast<StringIndex>(pex.string_table.size());
    pex.string_table.push_back(s);
    table[s] = idx;
    return idx;
  }
};

VariableData Id(Builder& b, const std::string& n) {
  return {VariableData::Type::kIdentifier, b.S(n), 0, 0.0f, false};
}
VariableData IntV(int v) {
  return {VariableData::Type::kInteger, kInvalidString, v, 0.0f, false};
}
VariableData StrV(Builder& b, const std::string& s) {
  return {VariableData::Type::kString, b.S(s), 0, 0.0f, false};
}

Instruction In(Op op, std::vector<VariableData> args, std::vector<VariableData> var = {}) {
  return {op, std::move(args), std::move(var)};
}

bool Has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

}  // namespace

int main() {
  Builder b;
  int failures = 0;
  auto check = [&](const char* what, bool ok) {
    std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
  };

  // int Foo(int n):  if (n > 0) return n  else return 0
  Function foo;
  foo.return_type = b.S("int");
  foo.params.push_back({b.S("n"), b.S("int")});
  foo.locals.push_back({b.S("::temp0"), b.S("bool")});
  foo.code = {
      In(Op::kCmpGt, {Id(b, "::temp0"), Id(b, "n"), IntV(0)}),
      In(Op::kJmpF, {Id(b, "::temp0"), IntV(3)}),  // false -> else at index 4
      In(Op::kReturn, {Id(b, "n")}),
      In(Op::kJmp, {IntV(2)}),  // over the else, to index 5 (end)
      In(Op::kReturn, {IntV(0)}),
  };

  // void Bar():  i = 0;  while (i < 10) { Debug.Notification("hi"); i = i + 1; }
  Function bar;
  bar.return_type = b.S("");  // none -> void
  bar.locals.push_back({b.S("i"), b.S("int")});
  bar.locals.push_back({b.S("::temp0"), b.S("bool")});
  bar.locals.push_back({b.S("::temp1"), b.S("int")});
  bar.locals.push_back({b.S("::temp2"), b.S("int")});
  bar.code = {
      In(Op::kAssign, {Id(b, "i"), IntV(0)}),
      In(Op::kCmpLt, {Id(b, "::temp0"), Id(b, "i"), IntV(10)}),  // loop header (index 1)
      In(Op::kJmpF, {Id(b, "::temp0"), IntV(5)}),                // exit -> index 7
      In(Op::kCallStatic,
         {Id(b, "Debug"), Id(b, "Notification"), Id(b, "::temp2")}, {StrV(b, "hi")}),
      In(Op::kIAdd, {Id(b, "::temp1"), Id(b, "i"), IntV(1)}),
      In(Op::kAssign, {Id(b, "i"), Id(b, "::temp1")}),
      In(Op::kJmp, {IntV(-5)}),  // back-edge to index 1
  };

  // void Classify(int n): if (n==1) A() elseif (n==2) B() else C(), the
  // if/elseif/else shape the compiler emits with no-op merge jumps, which the
  // structurer must rebuild as nested if/else (never a goto).
  Function classify;
  classify.return_type = b.S("");
  classify.params.push_back({b.S("n"), b.S("int")});
  classify.locals.push_back({b.S("::temp0"), b.S("bool")});
  classify.locals.push_back({b.S("::temp1"), b.S("int")});
  classify.code = {
      In(Op::kCmpEq, {Id(b, "::temp0"), Id(b, "n"), IntV(1)}),
      In(Op::kJmpF, {Id(b, "::temp0"), IntV(3)}),                                  // -> 4
      In(Op::kCallStatic, {Id(b, "Debug"), Id(b, "Trace"), Id(b, "::temp1")}, {StrV(b, "A")}),
      In(Op::kJmp, {IntV(6)}),                                                     // -> END(9)
      In(Op::kCmpEq, {Id(b, "::temp0"), Id(b, "n"), IntV(2)}),                     // 4
      In(Op::kJmpF, {Id(b, "::temp0"), IntV(3)}),                                  // -> 8
      In(Op::kCallStatic, {Id(b, "Debug"), Id(b, "Trace"), Id(b, "::temp1")}, {StrV(b, "B")}),
      In(Op::kJmp, {IntV(2)}),                                                     // -> END(9)
      In(Op::kCallStatic, {Id(b, "Debug"), Id(b, "Trace"), Id(b, "::temp1")}, {StrV(b, "C")}),  // 8
  };

  // void Touch(): reads the auto-property MyRef through its hidden backing slot
  // (::MyRef_var). The decompiler must route that read back to the property and
  // suppress the backing field.
  Function touch;
  touch.return_type = b.S("");
  touch.locals.push_back({b.S("::temp0"), b.S("ObjectReference")});
  touch.code = {
      In(Op::kCallMethod, {Id(b, "Enable"), Id(b, "::MyRef_var"), Id(b, "::temp0")}),
  };

  // bool Or(bool a, bool b): return a || b, the short-circuit form the compiler
  // emits, where one temp slot is written on both the skip and fall-through paths
  // and read at the merge. Every write must land in the SAME C# local, or the
  // skip path (a == true) reads an unassigned temp and the result flips.
  Function or_fn;
  or_fn.return_type = b.S("bool");
  or_fn.params.push_back({b.S("a"), b.S("bool")});
  or_fn.params.push_back({b.S("b"), b.S("bool")});
  or_fn.locals.push_back({b.S("::temp0"), b.S("bool")});
  or_fn.code = {
      In(Op::kAssign, {Id(b, "::temp0"), Id(b, "a")}),
      In(Op::kJmpT, {Id(b, "::temp0"), IntV(2)}),  // if a, skip (temp stays true)
      In(Op::kAssign, {Id(b, "::temp0"), Id(b, "b")}),
      In(Op::kReturn, {Id(b, "::temp0")}),
  };

  Object obj;
  obj.name = b.S("MyQuest");
  obj.parent_class = b.S("Quest");
  obj.doc_string = b.S("line one\nline two");  // multi-line doc must stay commented
  obj.variables.push_back({b.S("_count"), b.S("int"), 0, IntV(5)});
  obj.variables.push_back({b.S("::MyRef_var"), b.S("ObjectReference"), 0, {}});  // hidden backing

  Property prop;
  prop.name = b.S("MyRef");
  prop.type = b.S("ObjectReference");
  prop.flags = 0x7;  // readable | writable | auto
  prop.auto_var_name = b.S("::MyRef_var");
  obj.properties.push_back(prop);

  State def;
  def.name = b.S("");
  def.functions.push_back({b.S("Foo"), foo});
  def.functions.push_back({b.S("Bar"), bar});
  def.functions.push_back({b.S("Classify"), classify});
  def.functions.push_back({b.S("Touch"), touch});
  def.functions.push_back({b.S("Or"), or_fn});
  obj.states.push_back(def);
  b.pex.objects.push_back(obj);

  const std::string cs = TranspileToCSharp(b.pex);
  std::printf("---- emitted ----\n%s----\n", cs.c_str());

  check("class extends Quest", Has(cs, "public class MyQuest : Quest"));
  check("member field with initializer", Has(cs, "private int _count = 5;"));
  check("auto property", Has(cs, "public ObjectReference MyRef { get; set; }"));
  check("Foo signature", Has(cs, "public int Foo(int n)"));
  check("if reconstructed from JmpF", Has(cs, "if ((n > 0)) {"));
  check("else reconstructed", Has(cs, "} else {"));
  check("return inside then", Has(cs, "return n;"));
  check("return inside else", Has(cs, "return 0;"));
  check("Bar is void", Has(cs, "public void Bar()"));
  check("while loop reconstructed", Has(cs, "while (true) {"));
  check("loop exit test", Has(cs, "if (!((i < 10))) break;"));
  check("static call with string arg", Has(cs, "Debug.Notification(\"hi\");"));
  check("temp inlined into increment", Has(cs, "i = (i + 1);"));
  check("if/elseif/else: first compare", Has(cs, "(n == 1)"));
  check("if/elseif/else: nested compare", Has(cs, "(n == 2)"));
  check("if/elseif/else: trace A/B/C", Has(cs, "Debug.Trace(\"A\");") &&
                                            Has(cs, "Debug.Trace(\"B\");") &&
                                            Has(cs, "Debug.Trace(\"C\");"));
  check("auto-prop backing field suppressed", !Has(cs, "MyRef_var"));
  check("backing read routed to property", Has(cs, "MyRef.Enable();"));
  check("multi-line doc fully commented", Has(cs, "// line one") && Has(cs, "// line two"));
  check("short-circuit: writes share one slot",
        Has(cs, "_t0 = a;") && Has(cs, "_t0 = b;") && Has(cs, "return _t0;") && !Has(cs, "_t1 = b;"));
  check("no raw temp leaked", !Has(cs, "::temp"));
  check("no goto fallback needed", !Has(cs, "goto L"));

  std::printf("%s (%d failures)\n", failures ? "TRANSPILETEST FAILED" : "TRANSPILETEST PASSED",
              failures);
  return failures ? 1 : 0;
}
