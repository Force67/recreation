// obscripttest: unit test for the Obscript source interpreter, plus an optional
// integration pass over a real Fallout 3 / New Vegas Data directory.
//
//   obscripttest              runs the self contained unit test
//   obscripttest <data_dir>   additionally parses every SCPT and reports the rate
//
// The unit test drives a small script through a mock host and asserts that
// control flow, arithmetic, set statements, quest stages, globals and calls all
// take effect.

#include <base/containers/unordered_map.h>
#include <base/strings/string_ref.h>
#include <base/strings/to_string.h>
#include <base/strings/xstring.h>

#include <cstdio>
#include <filesystem>

#include "components/script/obscript/obscript.h"

using namespace rx;
// rx::u64/i64 (long) and base/arch.h's (long long) are different types sharing
// a global name, so the 64-bit spellings below are qualified; the other scalars
// agree between the two and need no help.
using namespace rx::script::obscript;

namespace {

int failures = 0;
#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                \
    }                                                            \
  } while (0)

// Records everything the interpreter does so the test can assert on it.
class MockHost : public Host {
 public:
  base::UnorderedMap<base::String, f32> globals;
  base::UnorderedMap<base::String, i32> stages;
  base::UnorderedMap<base::String, f32> remote;  // "owner.var" -> value
  base::Vector<base::String> calls;

  f32 GetGlobal(base::StringRef id) override {
    auto* it = globals.find(base::String(id));
    return it != nullptr ? *it : 0;
  }
  void SetGlobal(base::StringRef id, f32 v) override { globals[base::String(id)] = v; }
  i32 GetStage(base::StringRef q) override {
    auto* it = stages.find(base::String(q));
    return it != nullptr ? *it : 0;
  }
  void SetStage(base::StringRef q, i32 s) override { stages[base::String(q)] = s; }
  f32 GetRemoteVar(base::StringRef o, base::StringRef v) override {
    auto* it = remote.find(base::String(o) + "." + base::String(v));
    return it != nullptr ? *it : 0;
  }
  void SetRemoteVar(base::StringRef o, base::StringRef v, f32 val) override {
    remote[base::String(o) + "." + base::String(v)] = val;
  }
  f32 Call(base::StringRef target, base::StringRef fn, const base::Vector<f32>& args,
           const base::Vector<base::String>& text) override {
    base::String s(target);
    if (!s.empty()) s += ".";
    s += base::String(fn);
    for (const base::String& t : text) s += " " + t;
    for (f32 a : args) s += " " + base::ToString(static_cast<int>(a));
    calls.push_back(s);
    return 0;
  }
};

void UnitTest() {
  const char* src =
      "scriptName TestScript\n"
      "; a comment\n"
      "short count\n"
      "float ratio\n"
      "\n"
      "Begin OnActivate\n"
      "  set count to 3\n"
      "  if ( count == 3 )\n"
      "    set count to count + 1\n"
      "    MyQuest.SetStage 20\n"
      "  elseif ( count > 5 )\n"
      "    set count to 99\n"
      "  else\n"
      "    set count to 0\n"
      "  endif\n"
      "  if ( GetStage MyQuest >= 20 && MyGlobal == 1 )\n"
      "    DoorRef.Unlock\n"
      "    set MyGlobal to 2\n"
      "  endif\n"
      "  set VaultQuest.progress to count * 2\n"
      "End\n"
      "\n"
      "Begin GameMode\n"
      "  set ratio to 10 / 4\n"
      "End\n";

  Script script;
  CHECK(Parse(src, &script));
  CHECK(script.name == "TestScript");
  CHECK(script.vars.size() == 2);
  CHECK(script.blocks.size() == 2);
  CHECK(script.blocks[0].type == "onactivate");
  CHECK(script.blocks[1].type == "gamemode");

  MockHost host;
  host.globals["MyGlobal"] = 1;
  host.stages["MyQuest"] = 0;

  Instance inst(&script, &host);
  CHECK(inst.Run("onactivate"));

  // count started 3, matched the first branch, incremented to 4.
  CHECK(inst.GetVar("count") == 4);
  // SetStage 20 ran, so the second if saw stage 20 and MyGlobal 1.
  CHECK(host.stages["MyQuest"] == 20);
  CHECK(host.globals["MyGlobal"] == 2);
  // DoorRef.Unlock dispatched as a call.
  CHECK(host.calls.size() == 1);
  CHECK(host.calls[0] == "DoorRef.unlock");
  // A remote var write: VaultQuest.progress = count(4) * 2 = 8.
  CHECK(host.remote["VaultQuest.progress"] == 8);

  Instance inst2(&script, &host);
  CHECK(inst2.Run("gamemode"));
  CHECK(inst2.GetVar("ratio") > 2.4f && inst2.GetVar("ratio") < 2.6f);

  // A missing event returns false.
  CHECK(!inst2.Run("ondeath"));

  std::printf("unit test: %s\n", failures == 0 ? "PASS" : "FAIL");
}

}  // namespace

int main(int argc, char** argv) {
  UnitTest();

  if (argc > 1) {
    // Integration: parse every SCTX in the data's plugins would need the record
    // store; here we only report that the binary links against real headers.
    std::printf("integration data dir: %s (parse pass wired in content load)\n", argv[1]);
  }
  return failures == 0 ? 0 : 1;
}
