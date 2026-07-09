#include "quest/quest_graph.h"

#include <vector>

namespace rx::quest {

const QuestNode* QuestGraph::FindNode(i32 id) const {
  for (const QuestNode& n : nodes)
    if (n.id == id) return &n;
  return nullptr;
}

i64 QuestInstance::GetVar(const std::string& key, i64 fallback) const {
  auto it = vars_.find(key);
  return it == vars_.end() ? fallback : it->second;
}

void QuestInstance::Start(QuestActionSink& sink) { Advance(graph_->start_node, sink); }

bool QuestInstance::Advance(i32 node, QuestActionSink& sink) {
  if (entered_.count(node)) {
    active_.insert(node);  // re-advancing a done node just keeps it on the frontier
    return false;
  }
  Enter(node, sink);
  return true;
}

void QuestInstance::Enter(i32 node, QuestActionSink& sink) {
  entered_.insert(node);
  active_.insert(node);
  const u64 quest = graph_->handle;
  const QuestNode* n = graph_->FindNode(node);
  if (n && n->kind == NodeKind::kPhase && (!has_stage_ || node > current_stage_)) {
    current_stage_ = node;
    has_stage_ = true;
  }
  sink.OnEnterNode(quest, node);
  if (!n) return;
  for (const Action& a : n->on_enter) {
    switch (a.kind) {
      case ActionKind::kSetObjectiveDisplayed:
        sink.SetObjectiveDisplayed(quest, a.objective, a.flag);
        break;
      case ActionKind::kSetObjectiveCompleted:
        sink.SetObjectiveCompleted(quest, a.objective, a.flag);
        break;
      case ActionKind::kRunScriptFragment:
        sink.RunScriptFragment(quest, node, a.fragment);
        break;
      case ActionKind::kCompleteQuest:
        sink.CompleteQuest(quest);
        break;
      case ActionKind::kFailQuest:
        sink.FailQuest(quest);
        break;
    }
  }
}

bool QuestInstance::Tick(const ConditionContext& ctx, QuestActionSink& sink) {
  bool any = false;
  // Iterate to a fixed point: taking one transition can satisfy another. The
  // guard bounds chaining so cyclic condition data cannot spin forever.
  for (int guard = 0; guard < 64; ++guard) {
    bool fired = false;
    // Snapshot the frontier; Advance() mutates active_ underneath us.
    std::vector<i32> frontier(active_.begin(), active_.end());
    for (i32 from : frontier) {
      if (!active_.count(from)) continue;
      for (const Transition& t : graph_->transitions) {
        if (t.from != from || t.trigger != TriggerKind::kCondition) continue;
        if (entered_.count(t.to)) continue;  // edge already taken
        if (!Evaluate(t.condition, ctx)) continue;
        active_.erase(from);  // moved along the edge
        Advance(t.to, sink);
        fired = true;
        any = true;
        break;
      }
      if (fired) break;
    }
    if (!fired) break;
  }
  return any;
}

bool QuestInstance::PostEvent(const std::string& event, QuestActionSink& sink) {
  bool any = false;
  std::vector<i32> frontier(active_.begin(), active_.end());
  for (i32 from : frontier) {
    if (!active_.count(from)) continue;
    for (const Transition& t : graph_->transitions) {
      if (t.from != from || t.trigger != TriggerKind::kEvent) continue;
      if (t.event != event || entered_.count(t.to)) continue;
      active_.erase(from);
      Advance(t.to, sink);
      any = true;
      break;
    }
  }
  return any;
}

}  // namespace rx::quest
