#include "quest/quest_import.h"

#include <algorithm>
#include <vector>

namespace rx::quest {

QuestGraph BuildQuestGraph(const QuestDef& def,
                           const std::unordered_map<i32, std::string>& stage_fragments) {
  QuestGraph graph;
  graph.handle = def.handle;

  // Distinct stage indices, in ascending order. A stage index can carry several
  // QSDT log entries; collapse them to one node and let FindStage pick the
  // display text (last entry wins, as the journal shows the latest). Stages that
  // exist only as a fragment (no def entry) still get a node, so the script's
  // SetStage always lands on a real node.
  std::vector<i32> indices;
  auto add_index = [&](i32 index) {
    if (std::find(indices.begin(), indices.end(), index) == indices.end())
      indices.push_back(index);
  };
  for (const StageDef& s : def.stages) add_index(s.index);
  for (const auto& [stage, fn] : stage_fragments) add_index(stage);
  std::sort(indices.begin(), indices.end());

  // CompletionStage() is the lowest stage flagged complete; mark its node so the
  // graph is self-describing (the importer-faithful representation). Any stage
  // flagged complete gets the action, not just the lowest.
  for (i32 index : indices) {
    QuestNode node;
    node.id = index;
    node.kind = NodeKind::kPhase;
    if (const StageDef* sd = def.FindStage(index)) node.log_entry = sd->log_entry;

    if (auto it = stage_fragments.find(index); it != stage_fragments.end() && !it->second.empty()) {
      Action frag;
      frag.kind = ActionKind::kRunScriptFragment;
      frag.fragment = it->second;
      node.on_enter.push_back(std::move(frag));
    }

    // A stage index is "completing" if any of its QSDT entries set the flag.
    bool completes = false;
    for (const StageDef& s : def.stages)
      if (s.index == index && s.complete_quest) completes = true;
    if (completes) {
      Action done;
      done.kind = ActionKind::kCompleteQuest;
      node.on_enter.push_back(std::move(done));
    }

    graph.nodes.push_back(std::move(node));
  }

  // Opening stage: lowest stage carrying a fragment; failing that, the lowest
  // stage; failing that (a stageless quest), 0.
  graph.start_node = indices.empty() ? 0 : indices.front();
  for (i32 index : indices) {
    if (auto it = stage_fragments.find(index); it != stage_fragments.end() && !it->second.empty()) {
      graph.start_node = index;
      break;
    }
  }

  return graph;
}

}  // namespace rx::quest
