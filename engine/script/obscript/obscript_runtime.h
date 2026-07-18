#ifndef RECREATION_SCRIPT_OBSCRIPT_OBSCRIPT_RUNTIME_H_
#define RECREATION_SCRIPT_OBSCRIPT_OBSCRIPT_RUNTIME_H_

#include <memory>
#include <string>
#include <unordered_map>

#include "core/types.h"
#include "script/obscript/obscript.h"

namespace rx::bethesda {
class RecordStore;
}

namespace rx::script::obscript {

// Loads and catalogs the Obscript (SCPT) records of a Fallout 3 / New Vegas load
// order and runs them. Each SCPT keeps its source text (SCTX); the runtime parses
// every one at load, then instantiates and drives blocks on demand.
//
// This is the classic-game counterpart to the Papyrus ScriptSystem: those games
// predate Papyrus, so their gameplay logic is compiled Obscript, not .pex.
class Runtime {
 public:
  // Parses every SCPT in the store. Returns the number that parsed successfully.
  int Build(const bethesda::RecordStore& records);

  int script_count() const { return static_cast<int>(scripts_.size()); }
  int block_count() const { return block_count_; }

  // A parsed script by its object name (case sensitive as authored), or null.
  const Script* Find(const std::string& name) const;

  // Instantiates a script by name against `host` and runs its `event` block.
  // Returns false when the script or the block is absent.
  bool RunBlock(const std::string& name, std::string_view event, Host& host);

  // Logs a summary: script/block counts and the block-type histogram, then runs
  // a few representative scripts through a tracing host and logs their effects.
  // A headless way to confirm the parse + interpret path against real data.
  void Report() const;

 private:
  std::unordered_map<std::string, Script> scripts_;  // object name -> parsed script
  std::unordered_map<std::string, int> block_types_;  // event -> count, for the report
  int block_count_ = 0;
};

}  // namespace rx::script::obscript

#endif  // RECREATION_SCRIPT_OBSCRIPT_OBSCRIPT_RUNTIME_H_
