#ifndef RECREATION_QUEST_QUEST_IMPORT_H_
#define RECREATION_QUEST_QUEST_IMPORT_H_

#include <string>
#include <unordered_map>

#include "core/types.h"
#include "quest/quest_def.h"
#include "quest/quest_graph.h"

namespace rx::quest {

// Builds a native QuestGraph from a parsed Skyrim QUST definition and its
// stage->fragment map (the VMAD quest fragments, keyed by stage index).
//
// The result is the degenerate, fully-faithful import: one kPhase node per
// distinct stage index, advanced by a kRunScriptFragment on-enter action (the
// Papyrus fragment is what calls SetStage to chain on), plus a kCompleteQuest
// action on stages flagged complete. No declarative transitions are emitted --
// imported quests advance through their scripts until conditions are lifted onto
// edges (a later step). start_node is the lowest stage carrying a fragment, or
// the lowest stage otherwise, matching StartQuest's opening-stage choice.
QuestGraph BuildQuestGraph(const QuestDef& def,
                           const std::unordered_map<i32, std::string>& stage_fragments);

}  // namespace rx::quest

#endif  // RECREATION_QUEST_QUEST_IMPORT_H_
