#ifndef RECREATION_QUEST_QUEST_GRAPH_H_
#define RECREATION_QUEST_QUEST_GRAPH_H_

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/types.h"
#include "quest/condition.h"

namespace rx::quest {

// Engine-native quest representation: a directed graph of nodes (journal phases)
// joined by transitions whose triggers are declarative (conditions or events).
// It is a superset of Skyrim's flat stage model -- an imported QUST becomes a
// graph of phase nodes keyed by stage index, advanced by kRunScriptFragment
// on-enter actions (the Papyrus fragment calls Advance) with no declarative
// transitions, until conditions are lifted off the scripts. Native quests author
// the transitions directly and need no scripts at all.

enum class NodeKind : u8 {
  kStart,  // entry node; QuestInstance::Start enters it. Carries no journal text.
  kPhase,  // a journal stage. Its id is the Skyrim stage index for imported quests.
};

enum class ActionKind : u8 {
  kSetObjectiveDisplayed,
  kSetObjectiveCompleted,
  kRunScriptFragment,  // compat: invoke a Papyrus stage fragment by name
  kCompleteQuest,
  kFailQuest,
};

// A side effect run when a node is entered. Fields are read per kind:
//   kSetObjective*      -> objective, flag
//   kRunScriptFragment  -> fragment
//   kComplete/kFailQuest-> (none)
struct Action {
  ActionKind kind = ActionKind::kCompleteQuest;
  i32 objective = 0;
  bool flag = false;
  std::string fragment;  // Papyrus function for kRunScriptFragment
};

struct QuestNode {
  i32 id = 0;  // unique within the graph; the stage index for imported quests
  NodeKind kind = NodeKind::kPhase;
  std::string log_entry;  // journal text shown while this is the latest phase
  std::vector<Action> on_enter;
};

enum class TriggerKind : u8 {
  kCondition,  // auto-fires when the condition list passes (evaluated each Tick)
  kEvent,      // fires when a matching event is posted to the instance
};

struct Transition {
  i32 from = 0;
  i32 to = 0;
  TriggerKind trigger = TriggerKind::kCondition;
  ConditionList condition;  // for kCondition
  std::string event;        // for kEvent
};

// The compiled, immutable shape of one quest's graph. Built once (by the Skyrim
// importer or a native author) and shared by every QuestInstance of it.
struct QuestGraph {
  u64 handle = 0;
  i32 start_node = 0;
  std::vector<QuestNode> nodes;
  std::vector<Transition> transitions;

  const QuestNode* FindNode(i32 id) const;
};

// Receives the side effects a graph produces as an instance traverses it. The
// engine implements this over the QuestSystem/world; tests record the calls. The
// authoritative quest flags/objectives live behind this sink, not in the
// instance, so the instance stays a thin driver over the immutable graph.
class QuestActionSink {
 public:
  virtual ~QuestActionSink() = default;
  virtual void OnEnterNode(u64 quest, i32 node) {}
  virtual void SetObjectiveDisplayed(u64 quest, i32 objective, bool displayed) {}
  virtual void SetObjectiveCompleted(u64 quest, i32 objective, bool completed) {}
  virtual void RunScriptFragment(u64 quest, i32 node, const std::string& fragment) {}
  virtual void CompleteQuest(u64 quest) {}
  virtual void FailQuest(u64 quest) {}
};

// The live traversal state of one quest's graph: which nodes are active, which
// have been entered, and a small blackboard. Semantics:
//   * Advance(node) is additive -- it enters a node alongside any others, the
//     way Skyrim's SetStage just sets a stage. Re-entering is a no-op.
//   * a Condition/Event transition that fires leaves its source node (standard
//     state-machine progression) and enters its target.
class QuestInstance {
 public:
  explicit QuestInstance(const QuestGraph* graph) : graph_(graph) {}

  // Enters the start node and runs its on-enter actions via the sink.
  void Start(QuestActionSink& sink);

  // Makes `node` active and runs its on-enter actions once. Skyrim's SetStage
  // routes here. Returns true if it entered freshly, false if already entered.
  bool Advance(i32 node, QuestActionSink& sink);

  // Evaluates outgoing condition transitions from every active node, taking
  // satisfied ones until quiescent (chaining is bounded against cyclic data).
  // Returns true if any transition fired. Imported pure-fragment quests have no
  // condition transitions, so this is a cheap no-op for them.
  bool Tick(const ConditionContext& ctx, QuestActionSink& sink);

  // Posts a named event, taking any active matching kEvent transition.
  bool PostEvent(const std::string& event, QuestActionSink& sink);

  bool IsActive(i32 node) const { return active_.count(node) != 0; }
  bool WasEntered(i32 node) const { return entered_.count(node) != 0; }
  // Highest entered phase node id: the "current stage" for the journal / HUD.
  i32 CurrentStage() const { return current_stage_; }

  // Minimal typed blackboard for native quest variables and resolved aliases.
  void SetVar(const std::string& key, i64 value) { vars_[key] = value; }
  i64 GetVar(const std::string& key, i64 fallback = 0) const;

 private:
  void Enter(i32 node, QuestActionSink& sink);

  const QuestGraph* graph_;
  std::unordered_set<i32> active_;
  std::unordered_set<i32> entered_;
  std::unordered_map<std::string, i64> vars_;
  i32 current_stage_ = 0;
  bool has_stage_ = false;
};

}  // namespace rx::quest

#endif  // RECREATION_QUEST_QUEST_GRAPH_H_
