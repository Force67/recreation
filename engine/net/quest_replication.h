#ifndef RECREATION_NET_QUEST_REPLICATION_H_
#define RECREATION_NET_QUEST_REPLICATION_H_

#include <functional>
#include <optional>
#include <vector>

#include <base/containers/unordered_map.h>

#include "core/types.h"
#include "quest/quest_system.h"

namespace rx::net {

// Quest state replication. Unlike entity snapshots, quest progress is sparse,
// permanent, and must never be lost, so updates ride the reliable channel and
// carry only the bits a client cannot derive on its own. Stage and objective
// TEXT is never sent: clients resolve it locally from their own QuestDef, so a
// QuestUpdate is just handles, stages, and a handful of flags.

// A quest status tagged with the content domain it belongs to: 0 is the primary
// (rendered) game, 1..N the additional games loaded via --add-game. recreation
// runs several games at once and their form ids collide across games, so every
// replicated quest carries its domain to route to the right game on the client.
struct DomainQuestStatus {
  u8 domain = 0;
  quest::QuestStatus status;
};

// Serializes the replicated subset of `quests` (domain, handle, stage, running/
// active/complete, and per-objective index + displayed/completed bits) into a
// self-describing, bounds-checked little-endian blob.
std::vector<u8> EncodeQuestUpdate(const std::vector<DomainQuestStatus>& quests);

// Inverse of EncodeQuestUpdate. Fills only the replicated fields and leaves
// text (name/log_entry/objective text) empty; the caller resolves those from
// its own definitions. Returns nullopt on a truncated or corrupt blob, never
// reading out of bounds.
std::optional<std::vector<DomainQuestStatus>> DecodeQuestUpdate(ByteSpan data);

// Server side: turns successive full quest snapshots into reliable deltas. Each
// Build call compares the live snapshot against the per-handle revision last
// sent and emits only the quests that changed, so an idle session produces an
// empty blob. ForceFull makes the next Build resend every quest (used when a
// client joins and needs the whole journal at once).
class QuestReplicator {
 public:
  // Returns the encoding of the quests that changed since the last Build, or an
  // empty vector when nothing changed. `snapshot` is the authoritative state
  // this tick across every domain (each game's QuestSystem::AllStatuses(),
  // tagged with its domain).
  std::vector<u8> Build(const std::vector<DomainQuestStatus>& snapshot);

  // The next Build resends the full snapshot regardless of revisions.
  void ForceFull() { force_full_ = true; }

 private:
  // Last revision replicated per (domain, handle), so unchanged quests drop out.
  // Quest handles use at most 48 bits (plugin<<32 | local), leaving the high
  // bits free to fold in the domain without collision.
  static u64 RevisionKey(u8 domain, quest::QuestHandle handle) {
    return (static_cast<u64>(domain) << 48) | handle;
  }
  base::UnorderedMap<u64, u32> sent_revision_;
  bool force_full_ = false;
};

// Client side: decodes a QuestUpdate blob and hands each quest to `sink` with
// its domain, which the engine routes to that game's QuestSystem::ApplyStatus.
// Returns false on a corrupt blob (the sink is then not called for any quest in
// it). A blob encoding zero quests is valid and simply invokes the sink zero
// times.
bool ApplyQuestUpdate(
    ByteSpan data,
    const std::function<void(u8 domain, const quest::QuestStatus&)>& sink);

}  // namespace rx::net

#endif  // RECREATION_NET_QUEST_REPLICATION_H_
