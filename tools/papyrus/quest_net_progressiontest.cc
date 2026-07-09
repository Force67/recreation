// quest_net_progressiontest: an end-to-end check that a server's quest
// progression replicates to a client over the QuestReplicator delta path.
// A server-side QuestSystem is driven stage by stage (start, advance, display
// then complete an objective, reach the completing stage) and after each
// mutation its AllStatuses() snapshot is run through QuestReplicator::Build and
// ApplyQuestUpdate into a SEPARATE client-side QuestSystem. The client must
// mirror the server at every step. No game data and no sockets are needed
// (QuestDef is built by hand, and the replicator/apply operate on byte blobs
// directly), so it runs in the ctest gate.

#include <cstdio>
#include <optional>
#include <vector>

#include "core/types.h"
#include "net/quest_replication.h"
#include "quest/quest_def.h"
#include "quest/quest_system.h"

// zetanet's headers (pulled in via net/quest_replication.h) inject their own
// arch_types scalar aliases, so the scalar types stay fully qualified as rx::
// throughout rather than imported, which would make u8/u64/i32 ambiguous.
namespace {

using rx::net::ApplyQuestUpdate;
using rx::net::DomainQuestStatus;
using rx::net::QuestReplicator;
using rx::quest::ObjectiveDef;
using rx::quest::ObjectiveStatus;
using rx::quest::QuestDef;
using rx::quest::QuestHandle;
using rx::quest::QuestStatus;
using rx::quest::QuestSystem;
using rx::quest::StageDef;

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

// An MQ101-shaped definition built by hand: three stages (200 completes the
// quest) and one objective with display text. Both the server and the client
// load it, so the client resolves text locally while only the bits travel.
QuestDef MakeUnboundDef(QuestHandle handle) {
  QuestDef def;
  def.handle = handle;
  def.editor_id = "MQ101";
  def.name = "Unbound";
  def.priority = 50;
  def.stages.push_back(StageDef{0, "Imperials ambushed the Stormcloaks.", false});
  def.stages.push_back(StageDef{10, "Escape Helgen with Ralof or Hadvar.", false});
  def.stages.push_back(StageDef{200, "Escaped Helgen.", /*complete_quest=*/true});
  ObjectiveDef obj;
  obj.index = 10;
  obj.text = "Escape Helgen Keep";
  def.objectives.push_back(std::move(obj));
  return def;
}

// Replicates one server tick into the client: Build the delta from the server's
// authoritative snapshot, apply it to the client, and report the delta size so
// callers can assert what (if anything) went out this tick. A zero return means
// nothing was sent.
size_t Replicate(QuestReplicator& rep, const QuestSystem& server,
                 QuestSystem& client) {
  // This single-game harness replicates everything as the primary domain (0).
  std::vector<DomainQuestStatus> snapshot;
  for (QuestStatus& s : server.AllStatuses()) snapshot.push_back({0, std::move(s)});
  std::vector<rx::u8> blob = rep.Build(snapshot);
  if (blob.empty()) return 0;
  size_t count = 0;
  const bool ok = ApplyQuestUpdate(blob, [&](rx::u8 /*domain*/, const QuestStatus& q) {
    client.ApplyStatus(q);
    ++count;
  });
  Check("delta applied without corruption", ok);
  return count;
}

void TestProgressionReplicates() {
  std::puts("quest progression replicates server -> client:");
  const QuestHandle h = 0x000a1234ull;

  QuestSystem server;
  server.SetDefinition(MakeUnboundDef(h));

  // The client loads the same definition so it resolves text locally, but holds
  // no live state until the server's deltas arrive.
  QuestSystem client;
  client.SetDefinition(MakeUnboundDef(h));

  QuestReplicator rep;

  // Tick 0: the quest has not been touched yet, so nothing replicates.
  Check("idle session sends nothing", Replicate(rep, server, client) == 0);
  Check("client has no quest yet", !client.IsRunning(h));

  // Tick 1: server starts the quest at its opening stage.
  server.StartQuest(h);
  server.SetStage(h, 0);
  Check("start replicates one quest", Replicate(rep, server, client) == 1);
  Check("client running after start", client.IsRunning(h));
  Check("client stage 0", client.GetStage(h) == 0);
  Check("client not complete", !client.IsComplete(h));

  // Re-running with no server change must produce an empty delta: the
  // revision-based replicator never resends an unchanged quest.
  Check("unchanged tick sends nothing", Replicate(rep, server, client) == 0);

  // Tick 2: server advances a stage and displays the objective.
  server.SetStage(h, 10);
  server.SetObjectiveDisplayed(h, 10, true);
  Check("advance replicates one quest", Replicate(rep, server, client) == 1);
  Check("client stage 10", client.GetStage(h) == 10);
  Check("client objective displayed", client.IsObjectiveDisplayed(h, 10));
  Check("client objective not yet completed", !client.IsObjectiveCompleted(h, 10));
  // Client resolved the objective text from its own definition, not the wire.
  QuestStatus mid = client.Status(h);
  Check("client resolves objective text locally",
        mid.objectives.size() == 1 && mid.objectives[0].index == 10 &&
            mid.objectives[0].text == "Escape Helgen Keep" &&
            mid.objectives[0].displayed && !mid.objectives[0].completed);
  Check("client log text resolved locally",
        mid.log_entry == "Escape Helgen with Ralof or Hadvar.");

  // Tick 3: server completes the objective.
  server.SetObjectiveCompleted(h, 10, true);
  Check("objective completion replicates", Replicate(rep, server, client) == 1);
  Check("client objective completed", client.IsObjectiveCompleted(h, 10));
  Check("client still running before completion stage", client.IsRunning(h));

  // Tick 4: server sets the completing stage, so IsComplete is true on both
  // ends after replication.
  server.SetStage(h, 200);
  Check("server complete after stage 200", server.IsComplete(h));
  Check("completion stage replicates", Replicate(rep, server, client) == 1);
  Check("client stage 200", client.GetStage(h) == 200);
  Check("client mirrors completion", client.IsComplete(h));

  // The fully-replicated client status matches the server's replicated view.
  QuestStatus cs = client.Status(h);
  QuestStatus ss = server.Status(h);
  Check("client mirrors server stage", cs.stage == ss.stage);
  Check("client mirrors server complete flag", cs.complete && ss.complete);
  Check("client mirrors server running flag", cs.running == ss.running);
  Check("client objective mirrors server",
        cs.objectives.size() == 1 && cs.objectives[0].displayed &&
            cs.objectives[0].completed);

  // The completed quest still surfaces in the client's running journal (the
  // server has not stopped it), carrying the completed objective.
  std::vector<QuestStatus> running = client.RunningStatuses();
  Check("completed quest still running on client",
        running.size() == 1 && running[0].handle == h && running[0].complete &&
            running[0].objectives.size() == 1 &&
            running[0].objectives[0].completed);

  // A final idle tick after completion must again replicate nothing.
  Check("post-completion idle sends nothing", Replicate(rep, server, client) == 0);
}

// A client that joins after the quest is already complete must receive the
// whole journal at once via ForceFull, even though no revision changed since
// the original client was caught up.
void TestLateJoinerBootstrap() {
  std::puts("late joiner bootstrap via ForceFull:");
  const QuestHandle h = 0x000b5678ull;

  QuestSystem server;
  server.SetDefinition(MakeUnboundDef(h));
  server.StartQuest(h);
  server.SetStage(h, 10);
  server.SetObjectiveDisplayed(h, 10, true);
  server.SetObjectiveCompleted(h, 10, true);
  server.SetStage(h, 200);
  Check("server quest is complete", server.IsComplete(h));

  // One replicator has already streamed the full state to an existing client,
  // so a steady-state Build would now be empty.
  QuestReplicator rep;
  QuestSystem existing;
  existing.SetDefinition(MakeUnboundDef(h));
  Check("existing client caught up", Replicate(rep, server, existing) == 1);
  Check("steady state empty", Replicate(rep, server, existing) == 0);

  // A fresh client joins: ForceFull makes the next Build resend everything so
  // the joiner gets the whole journal even with no new server mutation.
  QuestSystem joiner;
  joiner.SetDefinition(MakeUnboundDef(h));
  rep.ForceFull();
  Check("ForceFull resends full state", Replicate(rep, server, joiner) == 1);
  Check("joiner running", joiner.IsRunning(h));
  Check("joiner stage 200", joiner.GetStage(h) == 200);
  Check("joiner complete", joiner.IsComplete(h));
  Check("joiner objective completed", joiner.IsObjectiveCompleted(h, 10));

  QuestStatus js = joiner.Status(h);
  Check("joiner objective text resolved locally",
        js.objectives.size() == 1 && js.objectives[0].text == "Escape Helgen Keep" &&
            js.objectives[0].displayed && js.objectives[0].completed);

  // ForceFull is one-shot: the tick after the bootstrap is a delta again.
  Check("ForceFull does not stick", Replicate(rep, server, joiner) == 0);
}

}  // namespace

int main() {
  TestProgressionReplicates();
  TestLateJoinerBootstrap();
  if (g_failures == 0) {
    std::puts("quest_net_progression: all checks passed");
    return 0;
  }
  std::printf("quest_net_progression: %d checks FAILED\n", g_failures);
  return 1;
}
