// quest_aliastest: ParseQuestDefinition reads a quest's stages, objectives and
// alias fill rules from a hand-built QUST record. Covers the alias parsing the
// objective compass and ReferenceAlias.GetReference depend on (ALFR forced
// reference, ALUA unique actor, QSTA objective targets). No game data needed.

#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "bethesda/record.h"
#include "core/types.h"
#include "quest/quest_def.h"

namespace {

using rx::ByteSpan;
using rx::FourCc;
using rx::i32;
using rx::u32;
using rx::bethesda::Record;
using rx::bethesda::Subrecord;
using rx::quest::AliasDef;
using rx::quest::ObjectiveDef;
using rx::quest::ParseQuestDefinition;
using rx::quest::QuestDef;

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

// Owns each subrecord's bytes so the Record's non-owning ByteSpans stay valid.
// A deque keeps element addresses stable as more subrecords are appended.
struct QustBuilder {
  Record record;
  std::deque<std::vector<rx::u8>> store;

  void Add(u32 type, std::vector<rx::u8> bytes) {
    store.push_back(std::move(bytes));
    Subrecord s;
    s.type = type;
    s.data = ByteSpan(store.back().data(), store.back().size());
    record.subrecords.push_back(s);
  }
  void AddStr(u32 type, const std::string& s) {
    std::vector<rx::u8> b(s.begin(), s.end());
    b.push_back(0);
    Add(type, std::move(b));
  }
  template <typename T>
  void AddLe(u32 type, T value) {
    std::vector<rx::u8> b(sizeof(T));
    std::memcpy(b.data(), &value, sizeof(T));
    Add(type, std::move(b));
  }
};

}  // namespace

int main() {
  std::puts("quest definition alias parsing:");

  QustBuilder b;
  b.AddStr(FourCc('E', 'D', 'I', 'D'), "MQ101Test");
  b.AddStr(FourCc('F', 'U', 'L', 'L'), "Unbound");
  // Two stages: 10 (ordinary) and 200 (completes the quest, QSDT flag 0x01).
  b.AddLe<rx::u16>(FourCc('I', 'N', 'D', 'X'), 10);
  b.AddLe<rx::u8>(FourCc('Q', 'S', 'D', 'T'), 0);
  b.AddLe<rx::u16>(FourCc('I', 'N', 'D', 'X'), 200);
  b.AddLe<rx::u8>(FourCc('Q', 'S', 'D', 'T'), 1);
  // Objective 50 targets alias 3.
  b.AddLe<rx::u16>(FourCc('Q', 'O', 'B', 'J'), 50);
  b.AddStr(FourCc('N', 'N', 'A', 'M'), "Enter the Keep");
  b.AddLe<i32>(FourCc('Q', 'S', 'T', 'A'), 3);
  // Alias 3: a forced reference (ALFR). Alias 7: a unique actor (ALUA).
  b.AddLe<i32>(FourCc('A', 'L', 'S', 'T'), 3);
  b.AddLe<u32>(FourCc('A', 'L', 'F', 'R'), 0xDEADBEEFu);
  b.Add(FourCc('A', 'L', 'E', 'D'), {});
  b.AddLe<i32>(FourCc('A', 'L', 'S', 'T'), 7);
  b.AddLe<u32>(FourCc('A', 'L', 'U', 'A'), 0x000A1234u);
  b.Add(FourCc('A', 'L', 'E', 'D'), {});

  const QuestDef def = ParseQuestDefinition(/*handle=*/0x0003372bull, b.record, /*strings=*/nullptr);

  Check("editor id parsed", def.editor_id == "MQ101Test");
  Check("name parsed", def.name == "Unbound");

  Check("two stages parsed", def.stages.size() == 2);
  Check("completing stage is 200", def.CompletionStage() == 200);
  Check("stage 10 is not completing",
        def.FindStage(10) != nullptr && !def.FindStage(10)->complete_quest);

  const ObjectiveDef* obj = def.FindObjective(50);
  Check("objective 50 parsed", obj != nullptr);
  Check("objective text parsed", obj && obj->text == "Enter the Keep");
  Check("objective target alias parsed",
        obj && obj->target_aliases.size() == 1 && obj->target_aliases[0] == 3);

  Check("two aliases parsed", def.aliases.size() == 2);
  const AliasDef* forced = def.FindAlias(3);
  Check("forced-ref alias found", forced != nullptr);
  Check("forced reference id parsed", forced && forced->forced_ref_raw == 0xDEADBEEFu);
  Check("forced alias has no unique actor", forced && forced->unique_actor_raw == 0);

  const AliasDef* unique = def.FindAlias(7);
  Check("unique-actor alias found", unique != nullptr);
  Check("unique actor base parsed", unique && unique->unique_actor_raw == 0x000A1234u);
  Check("unique alias has no forced ref", unique && unique->forced_ref_raw == 0);

  Check("missing alias yields null", def.FindAlias(99) == nullptr);

  if (g_failures == 0) {
    std::puts("quest_alias: all checks passed");
    return 0;
  }
  std::printf("quest_alias: %d checks FAILED\n", g_failures);
  return 1;
}
