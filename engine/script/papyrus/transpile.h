#ifndef RECREATION_SCRIPT_PAPYRUS_TRANSPILE_H_
#define RECREATION_SCRIPT_PAPYRUS_TRANSPILE_H_

#include <set>
#include <string>

#include "script/papyrus/pex.h"

namespace rec::script::papyrus {

// Collects what a compile-harness run needs an external stub to declare, so the
// generated C# can be fed to a real C# compiler with nothing else present. The
// caller accumulates one sink across a whole corpus and then emits the stub.
struct HarnessSink {
  std::set<std::string> declared_types;  // every class the corpus itself defines
  std::set<std::string> ref_types;       // types named in `is`/static calls
  std::set<std::string> static_calls;    // "Type.Method" pairs reached statically
};

// EXPERIMENTAL. Recompiles a parsed Papyrus program back into C# source that
// targets the recreation scripting SDK (the Form-derived Quest/Actor/... types
// in sdk/Engine). The point is to let a modder pull an existing quest script out
// of the shipped .pex archives and keep editing it as readable C# instead of
// hand-porting Papyrus.
//
// This is a decompiler, not a 1:1 disassembler: it inlines the compiler's
// temporaries back into expressions, rebuilds property/array/call syntax, and
// reconstructs if/else/while control flow where the bytecode is reducible. When
// it meets a flow graph it cannot structure (or a Fallout struct opcode it does
// not model yet), it falls back to a labelled goto rendering of that one
// function and tags it with a comment, so the output is always a faithful,
// compilable representation rather than a wrong-but-pretty guess.
struct TranspileOptions {
  // Namespace the emitted classes are placed in.
  std::string namespace_name = "Recreation.Decompiled";
  // When true, every function is preceded by its raw bytecode as a comment block
  // (handy for eyeballing how a body was reconstructed).
  bool emit_disasm_comments = false;

  // Emits a self-contained variant for feeding to a real C# compiler: engine
  // types collapse to `dynamic` (so every call resolves without the SDK present)
  // and `sink`, if set, gathers the handful of stub declarations still needed.
  // This is the validation path, not the form a modder edits.
  bool compile_harness = false;
  HarnessSink* sink = nullptr;

  // Like compile_harness, but the emitted classes derive from a recording base
  // (`Eng`) and seed their object members with recorders, so the script can be
  // *executed* against a mock engine that logs every call. Used by the run test
  // (`pex2cs --runtest`). Implies the harness dynamic typing.
  bool runner = false;
};

// Transpiles every object in pex into one C# compilation unit and returns it.
// Never fails: unmodelled constructs degrade to comments, not errors.
std::string TranspileToCSharp(const PexFile& pex, const TranspileOptions& opts = {});

// Transpiles one function into a self-contained `public static` C# method named
// cs_name (no class context, concrete types). Intended for pure functions:
// no engine calls, properties, or member access, so the result compiles and
// runs against nothing but the BCL. This is what the differential tester
// (`pex2cs --difftest`) executes side by side with the Papyrus VM.
std::string TranspileFunctionToCSharp(const PexFile& pex, const Function& fn,
                                      const std::string& cs_name);

}  // namespace rec::script::papyrus

#endif  // RECREATION_SCRIPT_PAPYRUS_TRANSPILE_H_
