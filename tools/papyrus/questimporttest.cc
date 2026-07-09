// questimporttest: checks that BuildQuestGraph turns a parsed Skyrim QUST
// definition into a faithful native graph -- one phase node per stage, fragments
// as on-enter actions, completion flagged, the right opening stage -- and that
// driving the graph reproduces the SetStage/StartQuest behavior. No game data.

#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/types.h"
#include "quest/quest_def.h"
#include "quest/quest_graph.h"
#include "quest/quest_import.h"

using namespace rx;
using namespace rx::quest;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

struct RecordingSink : QuestActionSink {
  std::vector<std::string> events;
  void OnEnterNode(u64, i32 node) override { events.push_back("enter " + std::to_string(node)); }
  void RunScriptFragment(u64, i32 node, const std::string& fn) override {
    events.push_back("frag " + std::to_string(node) + ":" + fn);
  }
  void CompleteQuest(u64) override { events.push_back("complete"); }
};

StageDef Stage(i32 index, std::string log, bool complete) {
  StageDef s;
  s.index = index;
  s.log_entry = std::move(log);
  s.complete_quest = complete;
  return s;
}

const QuestNode* Node(const QuestGraph& g, i32 id) { return g.FindNode(id); }

bool HasAction(const QuestNode* n, ActionKind kind) {
  if (!n) return false;
  for (const Action& a : n->on_enter)
    if (a.kind == kind) return true;
  return false;
}

void TestImportShape() {
  std::printf("import shape:\n");
  QuestDef def;
  def.handle = 0x0100ABCD;
  def.editor_id = "MQ101";
  def.stages.push_back(Stage(0, "", false));    // silent opening stage, no fragment
  def.stages.push_back(Stage(10, "Find it", false));
  def.stages.push_back(Stage(10, "Find it (revised)", false));  // duplicate index
  def.stages.push_back(Stage(200, "All done", true));           // completing stage

  std::unordered_map<i32, std::string> frags;
  frags[10] = "Fragment_1";
  frags[200] = "Fragment_9";

  QuestGraph g = BuildQuestGraph(def, frags);
  Check("handle carried over", g.handle == def.handle);
  Check("one node per distinct stage index", g.nodes.size() == 3);
  Check("duplicate index collapsed, latest log wins",
        Node(g, 10) && Node(g, 10)->log_entry == "Find it (revised)");
  Check("stage 10 runs its fragment", HasAction(Node(g, 10), ActionKind::kRunScriptFragment));
  Check("stage 0 has no fragment", !HasAction(Node(g, 0), ActionKind::kRunScriptFragment));
  Check("completing stage flagged", HasAction(Node(g, 200), ActionKind::kCompleteQuest));
  Check("non-completing stage not flagged", !HasAction(Node(g, 10), ActionKind::kCompleteQuest));
  Check("opening stage is lowest fragmented stage (10, not silent 0)", g.start_node == 10);
  Check("no declarative transitions for an imported quest", g.transitions.empty());
}

void TestImportDrives() {
  std::printf("imported graph drives like SetStage:\n");
  QuestDef def;
  def.handle = 7;
  def.stages.push_back(Stage(10, "a", false));
  def.stages.push_back(Stage(20, "b", false));
  def.stages.push_back(Stage(30, "c", true));
  std::unordered_map<i32, std::string> frags;
  frags[10] = "F10";
  frags[20] = "F20";
  frags[30] = "F30";

  QuestGraph g = BuildQuestGraph(def, frags);
  RecordingSink sink;
  QuestInstance inst(&g);

  inst.Start(sink);  // StartQuest kicks the opening stage
  Check("start runs the opening fragment", sink.events.size() >= 2 &&
                                               sink.events[0] == "enter 10" &&
                                               sink.events[1] == "frag 10:F10");
  Check("current stage tracks the journal", inst.CurrentStage() == 10);

  inst.Advance(20, sink);  // a fragment SetStage-ing to 20
  Check("advancing runs stage 20 fragment", sink.events.back() == "frag 20:F20");

  inst.Advance(30, sink);  // completing stage
  Check("completing stage runs its fragment then completes",
        sink.events[sink.events.size() - 2] == "frag 30:F30" && sink.events.back() == "complete");
  Check("current stage is the completing stage", inst.CurrentStage() == 30);

  size_t before = sink.events.size();
  inst.Advance(20, sink);  // re-setting a done stage is a no-op, as in the game
  Check("re-setting a done stage does nothing", sink.events.size() == before);
}

}  // namespace

int main() {
  std::printf("questimporttest\n");
  TestImportShape();
  TestImportDrives();
  if (g_failures) {
    std::printf("FAILED: %d check(s)\n", g_failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
