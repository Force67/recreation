#ifndef RECREATION_QUEST_QUEST_DEF_H_
#define RECREATION_QUEST_QUEST_DEF_H_

#include <string>
#include <vector>

#include "core/types.h"

namespace rx::bethesda {
struct Record;
class StringTable;
}  // namespace rx::bethesda

namespace rx::quest {

// One journal stage: the index the script sets, its journal log text, and
// whether reaching it completes the quest. Parsed from the QUST INDX/QSDT/CNAM
// subrecord run.
struct StageDef {
  i32 index = 0;
  std::string log_entry;     // CNAM journal text for this log entry
  bool complete_quest = false;  // QSDT flag 0x01
};

// One quest objective: its index, the displayed text, and the placed
// references (aliases) it points the compass at.
struct ObjectiveDef {
  i32 index = 0;
  std::string text;            // NNAM display text
  std::vector<i32> target_aliases;  // QSTA alias ids (compass targets)
};

// One quest alias (ALST/ALLS block). Holds the two fill rules that resolve to a
// concrete reference without runtime conditions: a forced reference (ALFR) and a
// unique-actor base (ALUA, whose single placed ACHR fills the alias). Both raw
// form ids are plugin-relative; resolve them against the quest's plugin.
struct AliasDef {
  i32 id = 0;                  // ALST/ALLS alias id, matches an objective's QSTA
  std::string name;            // ALID alias name (e.g. "City", "Fort"), for <Alias=> tokens
  u32 forced_ref_raw = 0;      // ALFR forced reference, 0 when none
  u32 unique_actor_raw = 0;    // ALUA unique-actor NPC_ base, 0 when none
  // "Find Matching Reference" aliases (ALFA present): filled at runtime with a
  // placed reference of `ref_type_raw` (an ALRT LocationRefType keyword) found in
  // a location, usually the quest's Fort/City location alias (ALFI find_in_parent
  // points at it). This binds the Civil War siege's Attacker*/Defender*/Marker*
  // slots to the fort's placed refs.
  bool find_matching = false;  // ALFA present
  u32 ref_type_raw = 0;        // ALRT LocationRefType form id, 0 when none
  i32 find_in_parent = -1;     // ALFI parent alias id to search within, -1 when none
};

// The static, display-facing shape of a quest, parsed once from its QUST
// record. The QuestSystem stores this beside the live state so snapshots can
// carry log/objective text without the records.
struct QuestDef {
  u64 handle = 0;  // packed GlobalFormId, matches the QuestSystem handle
  std::string editor_id;
  std::string name;  // FULL
  i32 priority = 0;
  bool start_game_enabled = false;  // DNAM flag 0x01: the engine starts it at load
  std::vector<StageDef> stages;
  std::vector<ObjectiveDef> objectives;
  std::vector<AliasDef> aliases;

  const StageDef* FindStage(i32 index) const;
  const ObjectiveDef* FindObjective(i32 index) const;
  const AliasDef* FindAlias(i32 id) const;
  // Lowest stage index flagged complete_quest, or -1 if none.
  i32 CompletionStage() const;
};

// Parses a QUST record into a QuestDef. `handle` is the quest's packed form id.
// `strings` resolves localized subrecords (CNAM/FULL hold string ids in a
// localized plugin, inline text otherwise); may be null. Always succeeds; an
// unscripted or sparse quest just yields empty stage/objective lists.
QuestDef ParseQuestDefinition(u64 handle, const bethesda::Record& record,
                              const bethesda::StringTable* strings);

}  // namespace rx::quest

#endif  // RECREATION_QUEST_QUEST_DEF_H_
