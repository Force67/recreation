// questtest: deterministic checks for the engine-side quest system and the
// QUST definition parser. No game data needed, so it runs in the ctest gate.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bethesda/record.h"
#include "core/types.h"
#include "quest/quest_def.h"
#include "quest/quest_system.h"

using namespace rx;
using namespace rx::quest;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

// Backing store for the synthetic record's subrecord spans; the spans point
// into these buffers, so they must outlive the record.
struct Buffers {
  std::vector<std::vector<u8>> store;
  ByteSpan Bytes(const void* p, size_t n) {
    auto& b = store.emplace_back(n);
    if (n) std::memcpy(b.data(), p, n);
    return ByteSpan(b.data(), b.size());
  }
  ByteSpan Str(const char* s) {
    return Bytes(s, std::strlen(s) + 1);  // zero terminated, like the plugin
  }
  ByteSpan U16(u16 v) { return Bytes(&v, sizeof(v)); }
  ByteSpan U8(u8 v) { return Bytes(&v, sizeof(v)); }
  ByteSpan I32(i32 v) { return Bytes(&v, sizeof(v)); }
};

void Add(bethesda::Record& r, u32 type, ByteSpan data) {
  bethesda::Subrecord sub;
  sub.type = type;
  sub.data = data;
  r.subrecords.push_back(std::move(sub));
}

// Builds an MQ101-shaped QUST record: a name, three stages (the last completes
// the quest), and two objectives with a compass target.
bethesda::Record MakeUnboundRecord(Buffers& buf) {
  bethesda::Record r;
  Add(r, FourCc('E', 'D', 'I', 'D'), buf.Str("MQ101"));
  Add(r, FourCc('F', 'U', 'L', 'L'), buf.Str("Unbound"));
  u8 dnam[4] = {0, 0, 50, 0};  // priority 50 at byte 2
  Add(r, FourCc('D', 'N', 'A', 'M'), buf.Bytes(dnam, sizeof(dnam)));

  Add(r, FourCc('I', 'N', 'D', 'X'), buf.U16(0));
  Add(r, FourCc('Q', 'S', 'D', 'T'), buf.U8(0));
  Add(r, FourCc('C', 'N', 'A', 'M'), buf.Str("Imperials ambushed the Stormcloaks."));

  Add(r, FourCc('I', 'N', 'D', 'X'), buf.U16(10));
  Add(r, FourCc('Q', 'S', 'D', 'T'), buf.U8(0));
  Add(r, FourCc('C', 'N', 'A', 'M'), buf.Str("Escape Helgen with Ralof or Hadvar."));

  Add(r, FourCc('I', 'N', 'D', 'X'), buf.U16(200));
  Add(r, FourCc('Q', 'S', 'D', 'T'), buf.U8(0x01));  // complete quest
  Add(r, FourCc('C', 'N', 'A', 'M'), buf.Str("Escaped Helgen."));

  Add(r, FourCc('Q', 'O', 'B', 'J'), buf.U16(10));
  Add(r, FourCc('N', 'N', 'A', 'M'), buf.Str("Escape Helgen Keep"));
  Add(r, FourCc('Q', 'S', 'T', 'A'), buf.I32(5));

  Add(r, FourCc('Q', 'O', 'B', 'J'), buf.U16(20));
  Add(r, FourCc('N', 'N', 'A', 'M'), buf.Str("Flee the dragon attack"));
  return r;
}

void TestParse() {
  std::puts("quest_def parse:");
  Buffers buf;
  bethesda::Record r = MakeUnboundRecord(buf);
  QuestDef def = ParseQuestDefinition(0x000a1234ull, r, nullptr);

  Check("handle preserved", def.handle == 0x000a1234ull);
  Check("editor id MQ101", def.editor_id == "MQ101");
  Check("name Unbound", def.name == "Unbound");
  Check("priority 50", def.priority == 50);
  Check("three stages", def.stages.size() == 3);
  Check("stage 200 completes quest", def.CompletionStage() == 200);
  const StageDef* s10 = def.FindStage(10);
  Check("stage 10 log text", s10 && s10->log_entry == "Escape Helgen with Ralof or Hadvar.");
  Check("two objectives", def.objectives.size() == 2);
  const ObjectiveDef* o10 = def.FindObjective(10);
  Check("objective 10 text", o10 && o10->text == "Escape Helgen Keep");
  Check("objective 10 target alias", o10 && o10->target_aliases.size() == 1 &&
                                          o10->target_aliases[0] == 5);
}

void TestState() {
  std::puts("quest_system state:");
  Buffers buf;
  bethesda::Record r = MakeUnboundRecord(buf);
  const QuestHandle h = 0x000a1234ull;

  QuestSystem qs;
  qs.SetDefinition(ParseQuestDefinition(h, r, nullptr));

  int started = 0, stage_changes = 0, objective_changes = 0;
  qs.AddListener([&](QuestHandle, QuestEvent e) {
    if (e == QuestEvent::kStarted) ++started;
    if (e == QuestEvent::kStageChanged) ++stage_changes;
    if (e == QuestEvent::kObjectiveChanged) ++objective_changes;
  });

  Check("untouched quest not running", !qs.IsRunning(h));
  Check("untouched quest active by default", qs.IsActive(h));

  qs.StartQuest(h);
  Check("running after start", qs.IsRunning(h));
  Check("started event fired", started == 1);

  Check("first SetStage runs fragment", qs.SetStage(h, 0));
  Check("re-set stage is a no-op", !qs.SetStage(h, 0));
  Check("stage is 0", qs.GetStage(h) == 0);
  Check("stage 0 done", qs.GetStageDone(h, 0));
  Check("not complete yet", !qs.IsComplete(h));

  qs.SetStage(h, 10);
  qs.SetObjectiveDisplayed(h, 10, true);
  Check("objective 10 displayed", qs.IsObjectiveDisplayed(h, 10));
  Check("objective change fired", objective_changes == 1);

  QuestStatus st = qs.Status(h);
  Check("status name Unbound", st.name == "Unbound");
  Check("status stage 10", st.stage == 10);
  Check("status log is stage 10 entry", st.log_entry == "Escape Helgen with Ralof or Hadvar.");
  Check("status objective text resolved",
        st.objectives.size() == 2 && st.objectives[0].index == 10 &&
            st.objectives[0].text == "Escape Helgen Keep" && st.objectives[0].displayed);

  // Reaching the completion stage marks the quest complete.
  qs.SetStage(h, 200);
  qs.SetObjectiveCompleted(h, 10, true);
  Check("complete after stage 200", qs.IsComplete(h));
  Check("status reports complete", qs.Status(h).complete);

  Check("stage change count", stage_changes == 3);

  // Running/active snapshots feed the HUD.
  auto running = qs.RunningStatuses();
  Check("one running quest", running.size() == 1 && running[0].handle == h);
  qs.SetActive(h, false);
  Check("inactive quest hidden from HUD", qs.RunningStatuses().empty());
  Check("inactive quest shown when asked", qs.RunningStatuses(true).size() == 1);

  Check("revision advanced", qs.revision() > 0);
}

void TestApplyRemote() {
  std::puts("quest_system apply (multiplayer):");
  Buffers buf;
  bethesda::Record r = MakeUnboundRecord(buf);
  const QuestHandle h = 0x000a1234ull;

  QuestSystem server;
  server.SetDefinition(ParseQuestDefinition(h, r, nullptr));
  server.StartQuest(h);
  server.SetStage(h, 10);
  server.SetObjectiveDisplayed(h, 10, true);

  // A client mirrors the server snapshot and recovers the same view.
  QuestSystem client;
  client.SetDefinition(ParseQuestDefinition(h, r, nullptr));
  int applied = 0;
  client.AddListener([&](QuestHandle, QuestEvent e) {
    if (e == QuestEvent::kApplied) ++applied;
  });
  client.ApplyStatus(server.Status(h));

  Check("apply event fired", applied == 1);
  Check("client running", client.IsRunning(h));
  Check("client stage matches", client.GetStage(h) == 10);
  Check("client objective mirrored", client.IsObjectiveDisplayed(h, 10));
  QuestStatus cs = client.Status(h);
  Check("client text resolved locally from def", cs.objectives.size() == 2 &&
                                                     cs.objectives[0].text == "Escape Helgen Keep");
}

// Regressions for review findings: a remotely-applied complete bit must hold
// even when the local current stage is not the completing stage, and an
// objective completed without ever being displayed (and without a definition
// entry) must still appear in the status.
void TestRemoteCompleteAndOrphanObjective() {
  std::puts("quest_system review fixes:");
  const QuestHandle h = 0x000a1234ull;

  // A snapshot says complete at a stage that is NOT the definition's completing
  // stage (server advanced past it). The client must still report complete.
  QuestSystem client;
  Buffers buf;
  bethesda::Record r = MakeUnboundRecord(buf);
  client.SetDefinition(ParseQuestDefinition(h, r, nullptr));
  QuestStatus remote;
  remote.handle = h;
  remote.running = false;
  remote.active = true;
  remote.complete = true;
  remote.stage = 999;  // not stage 200 (the completing stage)
  client.ApplyStatus(remote);
  Check("remote complete bit honored despite non-completing stage", client.IsComplete(h));

  // An objective completed but never displayed, with no definition entry, still
  // shows in the status.
  QuestSystem qs;
  qs.StartQuest(h);
  qs.SetObjectiveCompleted(h, 999, true);
  QuestStatus st = qs.Status(h);
  bool found = false;
  for (const ObjectiveStatus& o : st.objectives)
    if (o.index == 999 && o.completed && !o.displayed) found = true;
  Check("completed-but-undisplayed orphan objective surfaces", found);
}

}  // namespace

int main() {
  TestParse();
  TestState();
  TestApplyRemote();
  TestRemoteCompleteAndOrphanObjective();
  if (g_failures == 0) {
    std::puts("quest: all checks passed");
    return 0;
  }
  std::printf("quest: %d checks FAILED\n", g_failures);
  return 1;
}
