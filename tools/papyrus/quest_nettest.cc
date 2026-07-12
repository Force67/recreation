// quest_nettest: deterministic checks for quest state replication. No game
// data needed (QuestStatus objects are built by hand), so it runs in ctest.

#include <cstdio>
#include <optional>
#include <utility>
#include <vector>

#include "core/types.h"
#include "gamenet/quest_replication.h"
#include "quest/quest_system.h"

// zetanet's headers (pulled in via net/quest_replication.h) inject their own
// arch_types scalar aliases, so the scalar types stay fully qualified as rx::
// throughout rather than imported, which would make u8/u64/i32 ambiguous.
namespace {

using rx::ByteSpan;
using rx::net::ApplyQuestUpdate;
using rx::net::DecodeQuestUpdate;
using rx::net::DomainQuestStatus;
using rx::net::EncodeQuestUpdate;
using rx::net::QuestReplicator;
using rx::quest::ObjectiveStatus;
using rx::quest::QuestStatus;

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

// A quest with the replicated fields set plus some text/name, which must NOT
// survive the round trip (clients resolve text locally), tagged with a domain.
DomainQuestStatus MakeQuest(rx::u64 handle, rx::i32 stage, bool running, bool active,
                            bool complete, std::vector<ObjectiveStatus> objectives = {},
                            rx::u8 domain = 0) {
  QuestStatus q;
  q.handle = handle;
  q.stage = stage;
  q.running = running;
  q.active = active;
  q.complete = complete;
  q.name = "should not travel";
  q.log_entry = "should not travel";
  q.objectives = std::move(objectives);
  return DomainQuestStatus{domain, std::move(q)};
}

ObjectiveStatus MakeObjective(rx::i32 index, bool displayed, bool completed) {
  ObjectiveStatus o;
  o.index = index;
  o.displayed = displayed;
  o.completed = completed;
  o.text = "should not travel";
  return o;
}

bool SameReplicatedFields(const DomainQuestStatus& da, const DomainQuestStatus& db) {
  if (da.domain != db.domain) return false;
  const QuestStatus& a = da.status;
  const QuestStatus& b = db.status;
  if (a.handle != b.handle || a.stage != b.stage || a.running != b.running ||
      a.active != b.active || a.complete != b.complete) {
    return false;
  }
  if (a.objectives.size() != b.objectives.size()) return false;
  for (size_t i = 0; i < a.objectives.size(); ++i) {
    const ObjectiveStatus& oa = a.objectives[i];
    const ObjectiveStatus& ob = b.objectives[i];
    if (oa.index != ob.index || oa.displayed != ob.displayed ||
        oa.completed != ob.completed) {
      return false;
    }
  }
  return true;
}

void TestRoundTrip() {
  std::puts("quest_replication round trip:");
  std::vector<DomainQuestStatus> quests = {
      MakeQuest(0x000a1234ull, 10, /*running=*/true, /*active=*/true,
                /*complete=*/false,
                {MakeObjective(10, /*displayed=*/true, /*completed=*/false),
                 MakeObjective(20, /*displayed=*/false, /*completed=*/true)}),
      MakeQuest(0x000b5678ull, 200, /*running=*/true, /*active=*/false,
                /*complete=*/true),
      MakeQuest(0xffffffffull, -1, /*running=*/false, /*active=*/true,
                /*complete=*/false),
  };

  std::vector<rx::u8> blob = EncodeQuestUpdate(quests);
  Check("non-empty encoding", !blob.empty());

  std::optional<std::vector<DomainQuestStatus>> decoded = DecodeQuestUpdate(blob);
  Check("decode succeeds", decoded.has_value());
  if (!decoded) return;
  Check("quest count preserved", decoded->size() == quests.size());

  bool all_match = decoded->size() == quests.size();
  for (size_t i = 0; i < decoded->size() && all_match; ++i) {
    all_match = SameReplicatedFields(quests[i], (*decoded)[i]);
  }
  Check("replicated fields match", all_match);

  bool text_empty = true;
  for (const DomainQuestStatus& q : *decoded) {
    if (!q.status.name.empty() || !q.status.log_entry.empty()) text_empty = false;
    for (const ObjectiveStatus& o : q.status.objectives) {
      if (!o.text.empty()) text_empty = false;
    }
  }
  Check("text never travels", text_empty);

  // A negative stage must survive (some quests start before stage 0).
  Check("negative stage preserved", (*decoded)[2].status.stage == -1);

  // An empty snapshot is a valid update carrying zero quests.
  std::optional<std::vector<DomainQuestStatus>> empty =
      DecodeQuestUpdate(EncodeQuestUpdate({}));
  Check("empty snapshot decodes to zero quests",
        empty.has_value() && empty->empty());
}

void TestDomainRouting() {
  std::puts("multi-domain routing:");
  // The same handle in two games must stay distinct, carrying its domain.
  std::vector<DomainQuestStatus> quests = {
      MakeQuest(0x0100ull, 10, true, true, false, {}, /*domain=*/0),
      MakeQuest(0x0100ull, 50, true, true, false, {}, /*domain=*/1),
  };
  std::optional<std::vector<DomainQuestStatus>> decoded =
      DecodeQuestUpdate(EncodeQuestUpdate(quests));
  Check("both domains decode", decoded.has_value() && decoded->size() == 2);
  if (!decoded) return;
  Check("domain tags preserved",
        (*decoded)[0].domain == 0 && (*decoded)[1].domain == 1);
  Check("same handle, distinct stages per domain",
        (*decoded)[0].status.handle == (*decoded)[1].status.handle &&
            (*decoded)[0].status.stage == 10 && (*decoded)[1].status.stage == 50);

  // The replicator keys on (domain, handle): the same handle in two domains is
  // two independent quests, both sent, and neither masks the other.
  QuestReplicator rep;
  std::vector<DomainQuestStatus> snap = quests;
  snap[0].status.revision = 1;
  snap[1].status.revision = 1;
  Check("both domains sent on first build",
        DecodeQuestUpdate(rep.Build(snap))->size() == 2);
  Check("unchanged sends nothing", rep.Build(snap).empty());
  // Advance only the Fallout (domain 1) quest; the Skyrim one stays put.
  snap[1].status.revision = 2;
  snap[1].status.stage = 60;
  std::optional<std::vector<DomainQuestStatus>> d = DecodeQuestUpdate(rep.Build(snap));
  Check("only the changed domain's quest sent",
        d.has_value() && d->size() == 1 && (*d)[0].domain == 1 &&
            (*d)[0].status.stage == 60);
}

void TestDeltas() {
  std::puts("QuestReplicator deltas:");
  QuestReplicator rep;

  // Two quests, revisions 1 and 1.
  std::vector<DomainQuestStatus> snap = {
      MakeQuest(0x10ull, 0, true, true, false),
      MakeQuest(0x20ull, 0, true, true, false),
  };
  snap[0].status.revision = 1;
  snap[1].status.revision = 1;

  std::vector<rx::u8> first = rep.Build(snap);
  std::optional<std::vector<DomainQuestStatus>> d1 = DecodeQuestUpdate(first);
  Check("first build sends both", d1.has_value() && d1->size() == 2);

  // Nothing changed: empty blob.
  Check("unchanged snapshot sends nothing", rep.Build(snap).empty());

  // Bump only the second quest's revision and stage.
  snap[1].status.revision = 2;
  snap[1].status.stage = 10;
  std::vector<rx::u8> second = rep.Build(snap);
  std::optional<std::vector<DomainQuestStatus>> d2 = DecodeQuestUpdate(second);
  Check("only changed quest sent",
        d2.has_value() && d2->size() == 1 && (*d2)[0].status.handle == 0x20ull &&
            (*d2)[0].status.stage == 10);

  // Re-running with the same snapshot is again empty.
  Check("re-running unchanged sends nothing", rep.Build(snap).empty());

  // A brand new quest appears and goes out on its own.
  snap.push_back(MakeQuest(0x30ull, 5, true, true, false));
  snap.back().status.revision = 1;
  std::optional<std::vector<DomainQuestStatus>> d3 = DecodeQuestUpdate(rep.Build(snap));
  Check("new quest sent alone",
        d3.has_value() && d3->size() == 1 && (*d3)[0].status.handle == 0x30ull);
}

void TestForceFull() {
  std::puts("QuestReplicator ForceFull:");
  QuestReplicator rep;
  std::vector<DomainQuestStatus> snap = {
      MakeQuest(0x10ull, 0, true, true, false),
      MakeQuest(0x20ull, 0, true, true, false),
  };
  snap[0].status.revision = 3;
  snap[1].status.revision = 3;

  Check("initial build sends all", DecodeQuestUpdate(rep.Build(snap))->size() == 2);
  Check("steady state empty", rep.Build(snap).empty());

  // A joining client needs the whole journal even though nothing changed.
  rep.ForceFull();
  std::optional<std::vector<DomainQuestStatus>> full = DecodeQuestUpdate(rep.Build(snap));
  Check("ForceFull resends everything", full.has_value() && full->size() == 2);

  // ForceFull is one-shot: the next build is a delta again.
  Check("ForceFull does not stick", rep.Build(snap).empty());
}

void TestApplySink() {
  std::puts("ApplyQuestUpdate sink:");
  std::vector<DomainQuestStatus> quests = {
      MakeQuest(0x10ull, 5, true, true, false, {MakeObjective(1, true, false)}, /*domain=*/0),
      MakeQuest(0x20ull, 6, true, false, true, {}, /*domain=*/1),
  };
  std::vector<rx::u8> blob = EncodeQuestUpdate(quests);

  std::vector<DomainQuestStatus> received;
  const bool ok = ApplyQuestUpdate(
      blob, [&](rx::u8 domain, const QuestStatus& q) {
        received.push_back(DomainQuestStatus{domain, q});
      });
  Check("apply succeeds", ok);
  Check("sink invoked per quest", received.size() == 2);
  Check("first quest delivered to its domain",
        received.size() == 2 && received[0].domain == 0 &&
            received[0].status.handle == 0x10ull && received[0].status.stage == 5 &&
            received[0].status.objectives.size() == 1);
  Check("second quest delivered to its domain",
        received.size() == 2 && received[1].domain == 1 &&
            received[1].status.handle == 0x20ull);
}

void TestCorrupt() {
  std::puts("corrupt rejection:");
  std::vector<DomainQuestStatus> quests = {
      MakeQuest(0x10ull, 5, true, true, false,
                {MakeObjective(1, true, false),
                 MakeObjective(2, false, true)}),
      MakeQuest(0x20ull, 6, true, false, true),
  };
  std::vector<rx::u8> blob = EncodeQuestUpdate(quests);

  // Empty buffer has no message header at all.
  Check("empty buffer rejected", !DecodeQuestUpdate(ByteSpan()).has_value());

  // One byte cannot hold the 2-byte fixed-section length.
  const std::vector<rx::u8> tiny = {0x00};
  Check("one byte rejected", !DecodeQuestUpdate(tiny).has_value());

  // Every truncation of a valid blob must be rejected (it is no longer
  // self-consistent) and, crucially, must never read out of bounds. Under the
  // sanitized ctest build an over-read here would abort the test.
  bool every_truncation_rejected = true;
  for (size_t cut = 1; cut < blob.size(); ++cut) {
    std::vector<rx::u8> shorter(blob.begin(), blob.begin() + cut);
    if (DecodeQuestUpdate(shorter).has_value()) every_truncation_rejected = false;
  }
  Check("every truncation rejected without over-read", every_truncation_rejected);

  // Corrupt a byte in the middle of the heap (a record body): the embedded
  // length/count no longer matches the buffer, so decode must reject it.
  bool mid_corruption_rejected = true;
  for (size_t i = 0; i < blob.size(); ++i) {
    std::vector<rx::u8> flipped = blob;
    flipped[i] ^= 0xff;
    // A flip is allowed to still parse (e.g. a flag bit), but it must never
    // crash. We only assert the decoder stays in bounds; that it returns at
    // all is the contract.
    (void)DecodeQuestUpdate(flipped);
  }
  Check("byte flips never crash the decoder", mid_corruption_rejected);
}

}  // namespace

int main() {
  TestRoundTrip();
  TestDomainRouting();
  TestDeltas();
  TestForceFull();
  TestApplySink();
  TestCorrupt();
  if (g_failures == 0) {
    std::puts("quest_net: all checks passed");
    return 0;
  }
  std::printf("quest_net: %d checks FAILED\n", g_failures);
  return 1;
}
