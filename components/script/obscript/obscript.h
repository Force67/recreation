#ifndef RECREATION_SCRIPT_OBSCRIPT_OBSCRIPT_H_
#define RECREATION_SCRIPT_OBSCRIPT_OBSCRIPT_H_

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include "core/types.h"

// A source-level interpreter for classic Gamebryo Obscript, the gameplay
// scripting language of Oblivion, Fallout 3 and Fallout: New Vegas (the games
// predating Papyrus). Their SCPT records keep the full Obscript source text
// (SCTX) next to the compiled bytecode, so we run the source directly instead
// of decompiling the stack bytecode.
//
// Obscript is case insensitive and line oriented. A script is a name, a set of
// typed local variables, and one or more event blocks:
//
//   scriptName MyScript
//   short myVar
//   Begin OnActivate
//     if ( GetStage MyQuest < 10 )
//       set myVar to myVar + 1
//       MyQuest.SetStage 10
//     endif
//   End
//
// The interpreter covers the common gameplay surface: typed locals, set
// statements, if/elseif/else/endif, arithmetic/comparison/logical expressions,
// dotted external references (Quest.Var, Ref.Var), and function-call statements
// dispatched to a host. Unknown functions degrade to no-ops (logged once) so a
// partially covered script still runs the parts we understand.
namespace rx::script::obscript {

// A parsed script: reusable, immutable, shared by every instance.
struct Script {
  enum class VarKind : u8 { kShort, kInt, kFloat, kRef };
  struct Var {
    base::String name;  // lower cased for case insensitive lookup
    VarKind kind = VarKind::kShort;
  };
  struct Block {
    base::String type;                 // lower cased event name, e.g. "gamemode", "onactivate"
    base::String param;                // optional block filter (a form editor id), verbatim
    base::Vector<base::String> lines;  // statement lines, comments stripped
  };

  base::String name;  // as authored (ScriptName)
  base::Vector<Var> vars;
  base::Vector<Block> blocks;
  // A quest/effect script has no object bindings; only the type field of the
  // SCHR header distinguishes them, which the caller passes through.
};

// Parses Obscript source text. Returns false only when no ScriptName is found;
// unrecognized lines are kept verbatim and skipped at run time.
bool Parse(base::StringRef source, Script* out);

// The game-state surface the interpreter calls into. Every hook has a harmless
// default so a host can implement only what it needs. Values are floats, the
// single Obscript numeric type at run time (short/int/float all promote).
class Host {
 public:
  virtual ~Host() = default;

  // A global variable (GLOB) by editor id.
  virtual f32 GetGlobal(base::StringRef editor_id) { return 0; }
  virtual void SetGlobal(base::StringRef editor_id, f32 value) {}

  // A quest stage: GetStage / SetStage <quest>.
  virtual i32 GetStage(base::StringRef quest_editor_id) { return 0; }
  virtual void SetStage(base::StringRef quest_editor_id, i32 stage) {}

  // A remote script variable: `Quest.Var` reads and `set Ref.Var to x` writes.
  // `owner` is the quest/reference editor id, `var` the member name.
  virtual f32 GetRemoteVar(base::StringRef owner, base::StringRef var) { return 0; }
  virtual void SetRemoteVar(base::StringRef owner, base::StringRef var, f32 value) {}

  // A general function call, e.g. `Ref.Disable` or `ShowMessage MyMsg`. `target`
  // is the calling reference editor id ("" for the implicit self), `fn` the
  // function name (lower cased), `args` the evaluated numeric arguments, and
  // `text_args` the bare identifier arguments (message/form editor ids). Returns
  // the call's numeric result (0 when void or unknown).
  virtual f32 Call(base::StringRef target,
                   base::StringRef fn,
                   const base::Vector<f32>& args,
                   const base::Vector<base::String>& text_args) {
    return 0;
  }

  virtual void Log(base::StringRef message) {}
};

// A live script instance: a parsed Script plus its own local variable state.
class Instance {
 public:
  Instance(const Script* script, Host* host);

  // Runs the first block whose event matches `event` (lower cased, e.g.
  // "gamemode"). When `param` is non-empty a block's own param must match it
  // (the reference that fired an OnActivate/OnTrigger event). Returns true when
  // a block ran to completion.
  bool Run(base::StringRef event, base::StringRef param = {});

  f32 GetVar(base::StringRef name) const;
  void SetVar(base::StringRef name, f32 value);

  const Script& script() const { return *script_; }

 private:
  const Script* script_;
  Host* host_;
  base::UnorderedMap<base::String, f32> locals_;  // lower cased name -> value
};

}  // namespace rx::script::obscript

#endif  // RECREATION_SCRIPT_OBSCRIPT_OBSCRIPT_H_
