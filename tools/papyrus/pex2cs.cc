// pex2cs: EXPERIMENTAL Papyrus -> C# decompiler. Reads a .pex (either a loose
// file or one out of the game archives) and writes recreation-SDK C# so a quest
// script can be re-edited as C# instead of hand-ported.
//
//   pex2cs --file <path.pex> [out.cs] [--disasm]
//   pex2cs <data_dir> <ScriptName> [out.cs] [--disasm]
//   pex2cs --audit <data_dir>            measure structuring quality over every
//                                        scripts/*.pex the archives provide
//
// ScriptName is the object name without extension, e.g. "WIChangeLocation04";
// it is read from scripts/<name>.pex in whichever mounted archive provides it.
// With no out path the C# is written to stdout.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "asset/vfs.h"
#include "bethesda/archive.h"
#include "script/papyrus/opcode.h"
#include "script/papyrus/pex.h"
#include "script/papyrus/transpile.h"
#include "script/papyrus/value.h"
#include "script/papyrus/vm.h"

namespace {

using namespace rec;
using namespace rec::script::papyrus;

void MountArchives(asset::Vfs& vfs, const std::string& data_dir) {
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(data_dir, ec)) {
    std::string path = entry.path().string();
    if (auto provider = bethesda::OpenArchive(path)) vfs.Mount(std::move(provider));
  }
}

bool ReadFile(const std::string& path, std::vector<u8>* out) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return false;
  std::streamsize n = f.tellg();
  f.seekg(0);
  out->resize(static_cast<size_t>(n));
  return static_cast<bool>(f.read(reinterpret_cast<char*>(out->data()), n));
}

int Emit(const PexFile& pex, const std::string& out_path, bool disasm) {
  TranspileOptions opts;
  opts.emit_disasm_comments = disasm;
  std::string cs = TranspileToCSharp(pex, opts);
  if (out_path.empty()) {
    std::fputs(cs.c_str(), stdout);
    return 0;
  }
  std::ofstream f(out_path, std::ios::binary);
  if (!f) {
    std::fprintf(stderr, "cannot write %s\n", out_path.c_str());
    return 1;
  }
  f << cs;
  std::fprintf(stderr, "wrote %s (%zu objects, %zu bytes)\n", out_path.c_str(),
               pex.objects.size(), cs.size());
  return 0;
}

// Transpiles every scripts/*.pex in the archives and reports how much of the
// corpus reconstructs as structured C# (vs. the goto fallback), how many parse,
// and any unmodelled opcodes, the headline "how well does it work" numbers.
int Audit(const std::string& data_dir) {
  asset::Vfs vfs;
  MountArchives(vfs, data_dir);

  std::vector<std::string> scripts;
  vfs.Enumerate([&](std::string_view p) {
    if (p.size() > 4 && p.substr(0, 8) == "scripts/" &&
        p.substr(p.size() - 4) == ".pex")
      scripts.emplace_back(p);
  });

  size_t parsed = 0, failed = 0, funcs = 0, fallback = 0, unsupported = 0, fully = 0;
  for (const std::string& path : scripts) {
    auto data = vfs.Read(path);
    if (!data) continue;
    PexFile pex;
    if (!ParsePex(ByteSpan(data->data(), data->size()), &pex)) {
      ++failed;
      continue;
    }
    ++parsed;
    std::string cs = TranspileToCSharp(pex);
    size_t fb = 0, un = 0, pos = 0;
    while ((pos = cs.find("not reducible", pos)) != std::string::npos) { ++fb; pos += 8; }
    pos = 0;
    while ((pos = cs.find("unsupported opcode", pos)) != std::string::npos) { ++un; pos += 8; }
    // Count function bodies (method bodies open with ")\n    {" after a signature).
    size_t fn = 0, q = 0;
    while ((q = cs.find("    {\n", q)) != std::string::npos) { ++fn; q += 4; }
    funcs += fn;
    fallback += fb;
    unsupported += un;
    if (fb == 0 && un == 0) ++fully;
  }

  std::printf("\n==== pex2cs audit: %s ====\n", data_dir.c_str());
  std::printf("scripts found      : %zu\n", scripts.size());
  std::printf("parsed ok          : %zu\n", parsed);
  std::printf("parse failed       : %zu\n", failed);
  std::printf("fully structured   : %zu / %zu  (%.1f%%)\n", fully, parsed,
              parsed ? 100.0 * static_cast<double>(fully) / static_cast<double>(parsed) : 0.0);
  std::printf("function bodies    : %zu\n", funcs);
  std::printf("goto-fallback fns  : %zu  (%.2f%% of bodies)\n", fallback,
              funcs ? 100.0 * static_cast<double>(fallback) / static_cast<double>(funcs) : 0.0);
  std::printf("unsupported opcodes: %zu\n", unsupported);
  return 0;
}

// Writes every scripts/*.pex in the archives to <out_dir>/<Name>.cs, for bulk
// inspection or external (e.g. Roslyn) validation of the generated C#.
int DumpAll(const std::string& data_dir, const std::string& out_dir) {
  asset::Vfs vfs;
  MountArchives(vfs, data_dir);
  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);

  std::vector<std::string> scripts;
  vfs.Enumerate([&](std::string_view p) {
    if (p.size() > 12 && p.substr(0, 8) == "scripts/" && p.substr(p.size() - 4) == ".pex")
      scripts.emplace_back(p);
  });

  size_t written = 0;
  for (const std::string& path : scripts) {
    auto data = vfs.Read(path);
    if (!data) continue;
    PexFile pex;
    if (!ParsePex(ByteSpan(data->data(), data->size()), &pex)) continue;
    std::string stem = path.substr(8, path.size() - 12);  // strip "scripts/" + ".pex"
    std::ofstream f(out_dir + "/" + stem + ".cs", std::ios::binary);
    if (!f) continue;
    f << TranspileToCSharp(pex);
    ++written;
  }
  std::fprintf(stderr, "wrote %zu .cs files to %s\n", written, out_dir.c_str());
  return 0;
}

// Emits a compile-harness variant of every script into <out_dir>, plus a single
// __Stubs.cs declaring the handful of engine types/static calls the corpus still
// references. The directory then compiles with a stock C# compiler and no SDK,
// which checks the reconstruction semantically (control flow, scoping, returns,
// break/continue placement), a far stronger guarantee than a syntax parse.
int CompileCheck(const std::string& data_dir, const std::string& out_dir) {
  asset::Vfs vfs;
  MountArchives(vfs, data_dir);
  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);

  std::vector<std::string> scripts;
  vfs.Enumerate([&](std::string_view p) {
    if (p.size() > 12 && p.substr(0, 8) == "scripts/" && p.substr(p.size() - 4) == ".pex")
      scripts.emplace_back(p);
  });

  HarnessSink sink;
  TranspileOptions opts;
  opts.compile_harness = true;
  opts.sink = &sink;

  size_t written = 0;
  for (const std::string& path : scripts) {
    auto data = vfs.Read(path);
    if (!data) continue;
    PexFile pex;
    if (!ParsePex(ByteSpan(data->data(), data->size()), &pex)) continue;
    std::string stem = path.substr(8, path.size() - 12);
    std::ofstream f(out_dir + "/" + stem + ".cs", std::ios::binary);
    if (!f) continue;
    f << TranspileToCSharp(pex, opts);
    ++written;
  }

  // The harness funnels every engine call through two dynamic helpers, so the
  // whole external surface is this one tiny stub, no per-type guessing needed.
  std::ofstream stub(out_dir + "/__Stubs.cs", std::ios::binary);
  stub << "// Auto-generated harness stub: the only symbols the decompiled corpus\n"
          "// needs that it does not itself define. Everything is dynamic.\n"
          "namespace Recreation.Decompiled;\n\n"
          "public static class __Engine {\n"
          "    public static dynamic __Native(string sig, params object[] a) => null;\n"
          "    public static bool __Is(object o) => false;\n"
          "}\n";
  (void)sink;

  std::fprintf(stderr, "wrote %zu harness .cs + __Stubs.cs to %s\n", written, out_dir.c_str());
  return 0;
}

std::string LowerStr(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
bool IsPrimType(const std::string& t) {
  std::string b = LowerStr(t);
  if (b.size() >= 2 && b.compare(b.size() - 2, 2, "[]") == 0) b.resize(b.size() - 2);
  return b == "int" || b == "float" || b == "bool" || b == "string";
}
// A minimal C# identifier sanitizer matching the transpiler's (enough for a
// class name): non-alnum -> '_', leading digit prefixed.
std::string CsIdent(const std::string& n) {
  std::string out;
  for (char c : n) out.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
  if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) out.insert(out.begin(), '_');
  return out;
}

// Quest run test. Executes each of a quest's stage fragments both in the Papyrus
// VM and as compiled C#, against a mock engine that logs every call and hands
// back a neutral recorder, then compares the engine-call sequence. A match means
// the recompiled fragment drives the engine the way the bytecode does. A branch
// that hinges on a real engine return value can diverge, since a mock cannot
// reproduce the live game, so those are reported rather than hidden.

// Formats one call argument so the VM and C# sides render it identically: literal
// primitives compare exactly, objects collapse to a single token (their identity
// is seeded differently on each side and is not what this test checks).
std::string FormatArg(const rec::script::papyrus::Value& v) {
  using rec::script::papyrus::ValueType;
  switch (v.type()) {
    case ValueType::kInt: return "i" + std::to_string(v.as_int());
    case ValueType::kFloat:
      return "f" + std::to_string(static_cast<std::int64_t>(std::llround(v.as_float() * 1000.0)));
    case ValueType::kBool: return v.as_bool() ? "T" : "F";
    case ValueType::kString: return "s:" + v.as_string();
    case ValueType::kNone: return "_";
    default: return "o";  // object / array / struct
  }
}

// Registers a recorder for every native (type, function) declared anywhere in
// the corpus, so any engine call the quest makes resolves and is logged with its
// arguments into `trace`. Each returns a fresh object handle (deterministic by
// `counter`) so chained calls keep dispatching and both sides stay in lockstep.
void RegisterRecorders(asset::Vfs& vfs, rec::script::papyrus::NativeRegistry& reg,
                       std::shared_ptr<std::uint64_t> counter,
                       std::shared_ptr<std::vector<std::string>> trace) {
  using namespace rec::script::papyrus;
  std::set<std::pair<std::string, std::string>> seen;
  vfs.Enumerate([&](std::string_view p) {
    if (!(p.size() > 12 && p.substr(0, 8) == "scripts/" && p.substr(p.size() - 4) == ".pex"))
      return;
    auto data = vfs.Read(p);
    if (!data) return;
    PexFile pex;
    if (!ParsePex(ByteSpan(data->data(), data->size()), &pex)) return;
    for (const Object& o : pex.objects)
      for (const State& s : o.states)
        for (const NamedFunction& nf : s.functions)
          if (nf.function.is_native) seen.insert({pex.Str(o.name), pex.Str(nf.name)});
  });
  for (const auto& [type, func] : seen) {
    std::string fn = func;
    reg.Register(type, func,
                 [counter, trace, fn](VirtualMachine&, ObjectRef, std::vector<Value>& args) {
                   std::string call = fn + "(";
                   for (size_t i = 0; i < args.size(); ++i) call += (i ? "," : "") + FormatArg(args[i]);
                   trace->push_back(call + ")");
                   return Value::Object(ObjectRef{(*counter)++});
                 });
  }
}

const char* kEngRuntime = R"CS(using System;
using System.Collections.Generic;
using System.Dynamic;
namespace Recreation.Decompiled {
  // A universal recording stand-in for any engine object: every call, operator,
  // and conversion logs nothing but the call name and yields another recorder,
  // so a decompiled script runs to completion against it.
  public class Eng : DynamicObject {
    public static List<string> T = new List<string>();
    public string Tag;
    public Eng() { Tag = "?"; }
    public Eng(string t) { Tag = t; }
    // Render an argument exactly as the VM side does (Value formatting), so the
    // two argument lists compare character-for-character.
    public static string Fmt(object o) {
      if (o == null) return "_";
      if (o is Eng) return "o";
      if (o is bool bo) return bo ? "T" : "F";
      if (o is int i) return "i" + i;
      if (o is long l) return "i" + l;
      if (o is float f) return "f" + (long)Math.Round((double)f * 1000.0, MidpointRounding.AwayFromZero);
      if (o is double d) return "f" + (long)Math.Round(d * 1000.0, MidpointRounding.AwayFromZero);
      if (o is string s) return "s:" + s;
      return "o";
    }
    public static string Call(string name, object[] a) {
      var parts = new List<string>();
      foreach (var x in a) parts.Add(Fmt(x));
      return name + "(" + string.Join(",", parts) + ")";
    }
    public override bool TryInvokeMember(InvokeMemberBinder b, object[] a, out object r) {
      T.Add(Call(b.Name, a)); r = new Eng(b.Name); return true;
    }
    public override bool TryGetMember(GetMemberBinder b, out object r) { r = new Eng(b.Name); return true; }
    public override bool TrySetMember(SetMemberBinder b, object v) { return true; }
    public override bool TryGetIndex(GetIndexBinder b, object[] i, out object r) { r = new Eng("[]"); return true; }
    public override bool TrySetIndex(SetIndexBinder b, object[] i, object v) { return true; }
    public override bool TryBinaryOperation(BinaryOperationBinder b, object arg, out object r) { r = new Eng("op"); return true; }
    public override bool TryUnaryOperation(UnaryOperationBinder b, out object r) { r = new Eng("op"); return true; }
    public override bool TryConvert(ConvertBinder b, out object r) {
      var t = b.Type;
      if (t == typeof(bool)) r = true;
      else if (t == typeof(int)) r = 1;
      else if (t == typeof(long)) r = 1L;
      else if (t == typeof(float)) r = 1f;
      else if (t == typeof(double)) r = 1.0;
      else if (t == typeof(string)) r = "x";
      else r = this;
      return true;
    }
  }
  public static class __Engine {
    public static dynamic __Native(string sig, params object[] a) {
      int dot = sig.IndexOf('.');
      Eng.T.Add(Eng.Call(dot >= 0 ? sig.Substring(dot + 1) : sig, a));
      return new Eng(sig);
    }
    public static bool __Is(object o) { return false; }
  }
}
)CS";

int RunTest(const std::string& data_dir, const std::string& script_name, const std::string& out_dir) {
  using namespace rec::script::papyrus;
  asset::Vfs vfs;
  MountArchives(vfs, data_dir);
  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);

  auto blob = vfs.Read("scripts/" + script_name + ".pex");
  if (!blob) {
    std::fprintf(stderr, "not found: scripts/%s.pex\n", script_name.c_str());
    return 1;
  }
  PexFile pex;
  if (!ParsePex(ByteSpan(blob->data(), blob->size()), &pex) || pex.objects.empty()) {
    std::fprintf(stderr, "parse failed\n");
    return 1;
  }
  const Object& obj = pex.objects.front();
  std::string type_name = pex.Str(obj.name);

  // VM side: run each fragment, recording its engine calls.
  auto counter = std::make_shared<std::uint64_t>(1000000);
  auto trace = std::make_shared<std::vector<std::string>>();
  NativeRegistry reg;
  RegisterRecorders(vfs, reg, counter, trace);
  VirtualMachine vm(&reg);
  // Load the script and walk its ancestor chain so inherited functions resolve.
  vm.LoadScript(ByteSpan(blob->data(), blob->size()));
  for (std::string parent = vm.ParentClassOf(type_name); !parent.empty();) {
    auto pb = vfs.Read("scripts/" + parent + ".pex");
    if (!pb) break;
    std::string loaded = vm.LoadScript(ByteSpan(pb->data(), pb->size()));
    parent = loaded.empty() ? "" : vm.ParentClassOf(loaded);
  }
  ObjectRef inst = vm.CreateInstance(type_name);
  // Seed object-typed members with handles so calls on them resolve and log.
  for (const MemberVariable& v : obj.variables) {
    std::string vt = LowerStr(pex.Str(v.type));
    if (IsPrimType(vt)) continue;
    if (Value* slot = vm.MemberVar(inst, pex.Str(v.name)))
      *slot = Value::Object(ObjectRef{(*counter)++});
  }

  // Collect the quest's fragment functions (default state, no params).
  std::vector<std::string> fragments;
  for (const State& s : obj.states) {
    if (s.name != kInvalidString && !pex.Str(s.name).empty()) continue;
    for (const NamedFunction& nf : s.functions) {
      const std::string& fname = pex.Str(nf.name);
      if (fname.rfind("Fragment_", 0) == 0 && nf.function.params.empty()) fragments.push_back(fname);
    }
  }

  std::ofstream vm_out(out_dir + "/vm_trace.txt", std::ios::binary);
  for (const std::string& f : fragments) {
    trace->clear();
    vm.Call(inst, f, {});
    std::string line = f + "|";
    for (size_t i = 0; i < trace->size(); ++i) line += (i ? ";" : "") + (*trace)[i];
    vm_out << line << "\n";
  }

  // C# side: emit the recording runtime, the quest, and a driver.
  std::ofstream(out_dir + "/Eng.cs", std::ios::binary) << kEngRuntime;
  TranspileOptions ropts;
  ropts.runner = true;
  std::ofstream(out_dir + "/" + script_name + ".cs", std::ios::binary) << TranspileToCSharp(pex, ropts);

  std::ofstream prog(out_dir + "/Program.cs", std::ios::binary);
  prog << "using System;\nusing Recreation.Decompiled;\n"
          "class Program { static void Main(){\n"
          "    var q = new " << CsIdent(type_name) << "();\n";
  for (const std::string& f : fragments)
    prog << "    Eng.T.Clear(); try { q." << f << "(); } catch(Exception e){ Eng.T.Add(\"EXC:\"+e.GetType().Name); }"
            " Console.WriteLine(\"" << f << "|\"+string.Join(\";\", Eng.T));\n";
  prog << "}}\n";

  std::fprintf(stderr, "runtest %s: %zu fragments -> %s/{vm_trace.txt, Eng.cs, %s.cs, Program.cs}\n",
               script_name.c_str(), fragments.size(), out_dir.c_str(), script_name.c_str());
  return 0;
}

// Differential tester. Executes every pure function (no engine calls, no
// properties, no member access, primitive params and locals only) both in the
// Papyrus VM and as compiled C# over identical deterministic inputs, then diffs
// the results. A match across every trial is behavioural evidence that the
// reconstructed expressions and control flow compute what the bytecode does.

bool IsPrimScalar(const std::string& t) {
  std::string l = LowerStr(t);
  return l == "int" || l == "float" || l == "bool" || l == "string";
}

// A pure function: native-free, no engine reach, and every identifier it touches
// is one of its own params, locals, or compiler temps.
bool IsPure(const PexFile& pex, const Function& fn) {
  using namespace rec::script::papyrus;
  if (fn.is_native) return false;
  for (const TypedName& p : fn.params)
    if (!IsPrimScalar(LowerStr(pex.Str(p.type)))) return false;
  std::string rt = LowerStr(pex.Str(fn.return_type));
  if (!(rt.empty() || rt == "none" || IsPrimScalar(rt))) return false;
  std::set<std::string> names;
  for (const TypedName& p : fn.params) names.insert(pex.Str(p.name));
  for (const TypedName& l : fn.locals) {
    names.insert(pex.Str(l.name));
    if (!IsPrimType(pex.Str(l.type))) return false;
  }
  for (const Instruction& in : fn.code) {
    switch (in.op) {
      case Op::kCallMethod: case Op::kCallParent: case Op::kCallStatic:
      case Op::kPropGet: case Op::kPropSet: case Op::kIs:
      case Op::kStructCreate: case Op::kStructGet: case Op::kStructSet:
      case Op::kArrayFindStruct: case Op::kArrayRFindStruct:
      case Op::kArrayGetAllMatchingStructs:
        return false;
      default: break;
    }
    auto ok = [&](const std::vector<VariableData>& ops) {
      for (const VariableData& v : ops)
        if (v.type == VariableData::Type::kIdentifier && !names.count(pex.Str(v.string_index)))
          return false;
      return true;
    };
    if (!ok(in.args) || !ok(in.var_args)) return false;
  }
  return true;
}

std::int64_t DiffSeed(int fi, int t, int p) {
  return static_cast<std::int64_t>(fi) * 1000003 + static_cast<std::int64_t>(t) * 131 +
         static_cast<std::int64_t>(p) * 17 + 1;
}
int InpI(int fi, int t, int p) { return static_cast<int>(((DiffSeed(fi, t, p) % 41) + 41) % 41) - 20; }
double InpF(int fi, int t, int p) {
  return (static_cast<double>(((DiffSeed(fi, t, p) % 4001) + 4001) % 4001) - 2000.0) / 100.0;
}
bool InpB(int fi, int t, int p) { return (((DiffSeed(fi, t, p) % 2) + 2) % 2) == 0; }
const char* InpS(int fi, int t, int p) {
  static const char* kV[] = {"", "a", "Hello", "123"};
  return kV[((DiffSeed(fi, t, p) % 4) + 4) % 4];
}

// Formats a VM return value the same way the emitted C# formats its result.
std::string FormatResult(const std::string& ret_lower, const rec::script::papyrus::Value& v) {
  using rec::script::papyrus::Value;
  if (ret_lower == "int") return "I" + std::to_string(v.ToInt());
  if (ret_lower == "bool") return v.ToBool() ? "B1" : "B0";
  if (ret_lower == "string") return "S" + v.ToString();
  if (ret_lower == "float")
    return "F" + std::to_string(static_cast<std::int64_t>(std::llround(v.ToFloat() * 1000.0)));
  return "V";
}

int DiffTest(const std::string& data_dir, const std::string& out_dir) {
  using namespace rec::script::papyrus;
  asset::Vfs vfs;
  MountArchives(vfs, data_dir);
  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);

  std::vector<std::string> scripts;
  vfs.Enumerate([&](std::string_view p) {
    if (p.size() > 12 && p.substr(0, 8) == "scripts/" && p.substr(p.size() - 4) == ".pex")
      scripts.emplace_back(p);
  });
  std::sort(scripts.begin(), scripts.end());  // stable order so fi is reproducible

  constexpr int kTrials = 16;
  constexpr int kMaxFns = 6000;

  std::ofstream vm_out(out_dir + "/vm_results.txt", std::ios::binary);
  std::ofstream cs(out_dir + "/Diff.cs", std::ios::binary);
  cs << "using System;\nusing System.Globalization;\n"
        "static class Diff {\n"
        "  static long Sd(int fi,int t,int p){ return (long)fi*1000003 + (long)t*131 + (long)p*17 + 1; }\n"
        "  static int InpI(int fi,int t,int p){ return (int)(((Sd(fi,t,p)%41)+41)%41) - 20; }\n"
        "  static float InpF(int fi,int t,int p){ return (float)((double)(((Sd(fi,t,p)%4001)+4001)%4001) - 2000.0)/100.0f; }\n"
        "  static bool InpB(int fi,int t,int p){ return (((Sd(fi,t,p)%2)+2)%2)==0; }\n"
        "  static string InpS(int fi,int t,int p){ string[] v={\"\",\"a\",\"Hello\",\"123\"}; return v[((Sd(fi,t,p)%4)+4)%4]; }\n";

  std::string main_body;
  int fi = 0;
  for (const std::string& path : scripts) {
    if (fi >= kMaxFns) break;
    auto data = vfs.Read(path);
    if (!data) continue;
    PexFile pex;
    if (!ParsePex(ByteSpan(data->data(), data->size()), &pex)) continue;
    if (pex.objects.empty()) continue;

    // One VM per script, the script loaded once; pure functions ignore instance
    // state so a single instance serves every call.
    VirtualMachine vm(nullptr);
    PexFile pex_for_vm = pex;
    std::string type = vm.AddScript(std::move(pex_for_vm));
    if (type.empty()) continue;
    ObjectRef inst = vm.CreateInstance(type);

    const Object& obj = pex.objects.front();
    for (const State& s : obj.states) {
      if (s.name != kInvalidString && !pex.Str(s.name).empty()) continue;  // default state only
      for (const NamedFunction& nf : s.functions) {
        if (fi >= kMaxFns) break;
        const Function& fn = nf.function;
        if (!IsPure(pex, fn)) continue;
        std::string fname = pex.Str(nf.name);
        std::string rt = LowerStr(pex.Str(fn.return_type));

        // Emit the C# method and a per-function result printer.
        cs << TranspileFunctionToCSharp(pex, fn, "Func_" + std::to_string(fi));
        cs << "  static string R_" << fi << "(int t){ ";
        std::string call = "Func_" + std::to_string(fi) + "(";
        for (size_t p = 0; p < fn.params.size(); ++p) {
          if (p) call += ", ";
          std::string pt = LowerStr(pex.Str(fn.params[p].type));
          std::string fn_in = pt == "int" ? "InpI" : pt == "float" ? "InpF" : pt == "bool" ? "InpB" : "InpS";
          call += fn_in + "(" + std::to_string(fi) + ",t," + std::to_string(p) + ")";
        }
        call += ")";
        if (rt == "int") cs << "return \"I\"+(" << call << ");";
        else if (rt == "bool") cs << "return (" << call << ")?\"B1\":\"B0\";";
        else if (rt == "string") cs << "return \"S\"+(" << call << ");";
        else if (rt == "float") cs << "return \"F\"+((long)Math.Round((double)(" << call << ")*1000.0, MidpointRounding.AwayFromZero));";
        else cs << call << "; return \"V\";";
        cs << " }\n";
        main_body += "    for(int t=0;t<" + std::to_string(kTrials) + ";t++) Console.WriteLine(\"" +
                     std::to_string(fi) + "|\"+t+\"|\"+R_" + std::to_string(fi) + "(t));\n";

        // Run the same trials in the VM.
        for (int t = 0; t < kTrials; ++t) {
          std::vector<Value> args;
          for (size_t p = 0; p < fn.params.size(); ++p) {
            std::string pt = LowerStr(pex.Str(fn.params[p].type));
            if (pt == "int") args.push_back(Value::Int(InpI(fi, t, static_cast<int>(p))));
            else if (pt == "float") args.push_back(Value::Float(static_cast<float>(InpF(fi, t, static_cast<int>(p)))));
            else if (pt == "bool") args.push_back(Value::Bool(InpB(fi, t, static_cast<int>(p))));
            else args.push_back(Value::Str(InpS(fi, t, static_cast<int>(p))));
          }
          Value r = fn.is_global ? vm.CallGlobal(type, fname, args) : vm.Call(inst, fname, args);
          vm_out << fi << "|" << t << "|" << FormatResult(rt, r) << "\n";
        }
        ++fi;
      }
    }
  }

  cs << "  static void Main(){\n" << main_body << "  }\n}\n";
  std::fprintf(stderr, "difftest: %d pure functions, %d trials each -> %s/{vm_results.txt,Diff.cs}\n",
               fi, kTrials, out_dir.c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  bool disasm = false;
  for (auto it = args.begin(); it != args.end();) {
    if (*it == "--disasm") {
      disasm = true;
      it = args.erase(it);
    } else {
      ++it;
    }
  }

  if (args.size() >= 2 && args[0] == "--audit") return Audit(args[1]);
  if (args.size() >= 3 && args[0] == "--dump-all") return DumpAll(args[1], args[2]);
  if (args.size() >= 3 && args[0] == "--compile-check") return CompileCheck(args[1], args[2]);
  if (args.size() >= 3 && args[0] == "--difftest") return DiffTest(args[1], args[2]);
  if (args.size() >= 4 && args[0] == "--runtest") return RunTest(args[1], args[2], args[3]);

  std::vector<u8> blob;
  std::string out_path;

  if (!args.empty() && args[0] == "--file") {
    if (args.size() < 2) {
      std::fprintf(stderr, "usage: %s --file <path.pex> [out.cs] [--disasm]\n", argv[0]);
      return 2;
    }
    if (!ReadFile(args[1], &blob)) {
      std::fprintf(stderr, "cannot read %s\n", args[1].c_str());
      return 1;
    }
    if (args.size() > 2) out_path = args[2];
  } else if (args.size() >= 2) {
    asset::Vfs vfs;
    MountArchives(vfs, args[0]);
    std::string vpath = "scripts/" + args[1] + ".pex";
    auto data = vfs.Read(vpath);
    if (!data) {
      std::fprintf(stderr, "not found in archives: %s\n", vpath.c_str());
      return 1;
    }
    blob.assign(data->data(), data->data() + data->size());
    if (args.size() > 2) out_path = args[2];
  } else {
    std::fprintf(stderr,
                 "usage:\n  %s --file <path.pex> [out.cs] [--disasm]\n"
                 "  %s <data_dir> <ScriptName> [out.cs] [--disasm]\n",
                 argv[0], argv[0]);
    return 2;
  }

  PexFile pex;
  if (!ParsePex(ByteSpan(blob.data(), blob.size()), &pex)) {
    std::fprintf(stderr, "parse failed\n");
    return 1;
  }
  return Emit(pex, out_path, disasm);
}
