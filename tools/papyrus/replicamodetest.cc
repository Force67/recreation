// replicamodetest: checks that a multiplayer client (replica mode) ignores its
// own scripts' quest mutations and instead mirrors the server through
// QuestSystem::ApplyStatus, while a host / single-player applies them normally.
// No game data, so it runs in the ctest gate.

#include <cstdio>

#include "quest/quest_system.h"
#include "script/games/skyrim/skyrim_bindings.h"
#include "script/host/bridge.h"

using rec::quest::QuestStatus;
using rec::script::host::ManagedEvent;
using rec::script::host::ManagedEventId;
using rec::script::papyrus::ObjectRef;
using rec::script::skyrim::RecordBackedSkyrimBindings;

namespace {
int g_failures = 0;
void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}
}  // namespace

int main() {
  std::printf("replicamodetest\n");
  const ObjectRef quest{0x0100ABCD};

  // Authoritative (host / single-player): the local scripts drive quest state.
  {
    RecordBackedSkyrimBindings b;
    b.StartQuest(quest);
    b.SetStage(quest, 10);
    b.SetObjectiveDisplayed(quest, 1, true);
    Check("authoritative SetStage applies", b.GetStage(quest) == 10);
    Check("authoritative StartQuest runs the quest", b.IsRunning(quest));
    Check("authoritative objective change applies", b.IsObjectiveDisplayed(quest, 1));
  }

  // Replica (client): the same script calls must be no-ops...
  {
    RecordBackedSkyrimBindings b;
    b.set_replica_mode(true);
    b.StartQuest(quest);
    b.SetStage(quest, 10);
    b.SetObjectiveDisplayed(quest, 1, true);
    Check("replica SetStage is a no-op", b.GetStage(quest) == 0);
    Check("replica StartQuest is a no-op", !b.IsRunning(quest));
    Check("replica objective change is a no-op", !b.IsObjectiveDisplayed(quest, 1));

    // ...yet the client still mirrors the server's authoritative state.
    QuestStatus status;
    status.handle = quest.handle;
    status.running = true;
    status.stage = 20;
    b.quest_system().ApplyStatus(status);
    Check("replica mirrors server stage via ApplyStatus", b.GetStage(quest) == 20);
    Check("replica mirrors server running via ApplyStatus", b.IsRunning(quest));
  }

  // A replicated stage advance must also reach the managed layer, so C# questing
  // gameplay (XP rewards, the journal) fires on a client. ApplyReplicatedStatus
  // emits the managed event a local SetStage would; plain ApplyStatus does not.
  {
    RecordBackedSkyrimBindings b;
    b.set_replica_mode(true);
    int quest_events = 0;
    int last_stage = -1;
    b.set_event_sink([&](const ManagedEvent& e) {
      if (e.id == ManagedEventId::kQuestStageChanged) {
        ++quest_events;
        last_stage = e.i;
      }
    });

    QuestStatus status;
    status.handle = quest.handle;
    status.running = true;
    status.stage = 30;
    b.ApplyReplicatedStatus(status);
    Check("replicated apply mirrors the stage", b.GetStage(quest) == 30);
    Check("replicated apply emits the managed quest event", quest_events == 1);
    Check("the managed event carries the stage", last_stage == 30);

    // A periodic re-send of the same journal entry is not a new stage.
    b.ApplyReplicatedStatus(status);
    Check("re-applying the same stage does not re-fire", quest_events == 1);

    // A further advance fires again.
    status.stage = 40;
    b.ApplyReplicatedStatus(status);
    Check("a new replicated stage fires again", quest_events == 2);
    Check("the new stage is carried", last_stage == 40);
  }

  if (g_failures) {
    std::printf("FAILED: %d check(s)\n", g_failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
