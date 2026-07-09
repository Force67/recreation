// questgraphtest: deterministic checks for the native quest graph IR and its
// traversal (Start/Advance/Tick/PostEvent, on-enter actions, condition- and
// event-driven transitions). No game data needed, so it runs in the ctest gate.

#include <cstdio>
#include <string>
#include <vector>

#include "core/types.h"
#include "quest/condition.h"
#include "quest/quest_graph.h"

using namespace rx;
using namespace rx::quest;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

// Records every side effect the graph drives, so tests can assert ordering and
// content.
struct RecordingSink : QuestActionSink {
  std::vector<i32> entered;
  std::vector<std::string> log;
  bool completed = false;
  bool failed = false;

  void OnEnterNode(u64, i32 node) override { entered.push_back(node); }
  void SetObjectiveDisplayed(u64, i32 objective, bool displayed) override {
    log.push_back("disp " + std::to_string(objective) + "=" + (displayed ? "1" : "0"));
  }
  void SetObjectiveCompleted(u64, i32 objective, bool done) override {
    log.push_back("comp " + std::to_string(objective) + "=" + (done ? "1" : "0"));
  }
  void RunScriptFragment(u64, i32 node, const std::string& fn) override {
    log.push_back("frag " + std::to_string(node) + ":" + fn);
  }
  void CompleteQuest(u64) override { completed = true; }
  void FailQuest(u64) override { failed = true; }
};

// A condition context whose only knowledge is the quest's current stage, set by
// the test, so GetStage-based transitions can be driven deterministically.
struct StageContext : ConditionContext {
  float stage = 0.0f;
  float GetStage(u64) const override { return stage; }
};

QuestNode Phase(i32 id, std::string log) {
  QuestNode n;
  n.id = id;
  n.kind = NodeKind::kPhase;
  n.log_entry = std::move(log);
  return n;
}

Transition CondEdge(i32 from, i32 to, CompareOp op, float value) {
  Transition t;
  t.from = from;
  t.to = to;
  t.trigger = TriggerKind::kCondition;
  Comparison c;
  c.func = Func::kGetStage;
  c.op = op;
  c.value = value;
  t.condition.comparisons.push_back(c);
  return t;
}

void TestAdvanceAndActions() {
  std::printf("Advance + on-enter actions:\n");
  QuestGraph g;
  g.handle = 0xABCD;
  g.start_node = 10;
  QuestNode start = Phase(10, "Begin");
  Action frag;
  frag.kind = ActionKind::kRunScriptFragment;
  frag.fragment = "Fragment_0";
  start.on_enter.push_back(frag);
  g.nodes.push_back(start);

  QuestNode mid = Phase(20, "Middle");
  Action show;
  show.kind = ActionKind::kSetObjectiveDisplayed;
  show.objective = 1;
  show.flag = true;
  mid.on_enter.push_back(show);
  g.nodes.push_back(mid);

  QuestNode end = Phase(30, "Done");
  Action done;
  done.kind = ActionKind::kCompleteQuest;
  end.on_enter.push_back(done);
  g.nodes.push_back(end);

  RecordingSink sink;
  QuestInstance inst(&g);
  inst.Start(sink);
  Check("start entered the start node", inst.WasEntered(10) && inst.IsActive(10));
  Check("start ran its fragment", sink.log.size() == 1 && sink.log[0] == "frag 10:Fragment_0");
  Check("current stage is 10", inst.CurrentStage() == 10);

  Check("advance to 20 is fresh", inst.Advance(20, sink));
  Check("objective shown on enter", sink.log.back() == "disp 1=1");
  Check("current stage advanced to 20", inst.CurrentStage() == 20);

  Check("re-advancing 20 is a no-op", !inst.Advance(20, sink));
  Check("no extra action from re-advance", sink.log.back() == "disp 1=1");

  inst.Advance(30, sink);
  Check("completing stage fired CompleteQuest", sink.completed);
  Check("current stage is 30", inst.CurrentStage() == 30);
}

void TestConditionTransitions() {
  std::printf("Condition transitions:\n");
  QuestGraph g;
  g.handle = 1;
  g.start_node = 0;
  g.nodes.push_back(Phase(0, "a"));
  g.nodes.push_back(Phase(10, "b"));
  g.nodes.push_back(Phase(20, "c"));
  // 0 -> 10 when stage >= 5; 10 -> 20 when stage >= 15.
  g.transitions.push_back(CondEdge(0, 10, CompareOp::kGreaterOrEqual, 5.0f));
  g.transitions.push_back(CondEdge(10, 20, CompareOp::kGreaterOrEqual, 15.0f));

  StageContext ctx;
  RecordingSink sink;
  QuestInstance inst(&g);
  inst.Start(sink);

  ctx.stage = 1.0f;
  Check("no transition while condition is false", !inst.Tick(ctx, sink));
  Check("still on start node", inst.IsActive(0) && !inst.WasEntered(10));

  ctx.stage = 6.0f;
  Check("first edge fires when condition holds", inst.Tick(ctx, sink));
  Check("moved off source node", !inst.IsActive(0));
  Check("entered target node", inst.IsActive(10));
  Check("second edge did not fire yet", !inst.WasEntered(20));

  // Raising the stage past both thresholds chains 10 -> 20 in one Tick.
  ctx.stage = 20.0f;
  Check("second edge fires", inst.Tick(ctx, sink));
  Check("reached final node", inst.IsActive(20) && !inst.IsActive(10));
  Check("quiescent afterward", !inst.Tick(ctx, sink));
}

void TestEventTransitionAndBlackboard() {
  std::printf("Event transition + blackboard:\n");
  QuestGraph g;
  g.handle = 2;
  g.start_node = 0;
  g.nodes.push_back(Phase(0, "wait"));
  g.nodes.push_back(Phase(10, "go"));
  Transition t;
  t.from = 0;
  t.to = 10;
  t.trigger = TriggerKind::kEvent;
  t.event = "DoorOpened";
  g.transitions.push_back(t);

  RecordingSink sink;
  QuestInstance inst(&g);
  inst.Start(sink);

  Check("unrelated event does nothing", !inst.PostEvent("Other", sink));
  Check("matching event takes the edge", inst.PostEvent("DoorOpened", sink));
  Check("entered the event target", inst.IsActive(10) && !inst.IsActive(0));

  inst.SetVar("kills", 3);
  Check("blackboard stores and reads", inst.GetVar("kills") == 3);
  Check("blackboard fallback for missing key", inst.GetVar("absent", -1) == -1);
}

}  // namespace

int main() {
  std::printf("questgraphtest\n");
  TestAdvanceAndActions();
  TestConditionTransitions();
  TestEventTransitionAndBlackboard();
  if (g_failures) {
    std::printf("FAILED: %d check(s)\n", g_failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
