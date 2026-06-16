#ifndef RECREATION_QUEST_QUEST_SYSTEM_H_
#define RECREATION_QUEST_QUEST_SYSTEM_H_

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/types.h"
#include "quest/quest_def.h"

namespace rec::quest {

// A quest is addressed by its packed GlobalFormId (plugin << 32 | local_id),
// the same value the Papyrus guest uses as a quest instance handle.
using QuestHandle = u64;

// What changed about a quest, delivered to listeners. The engine uses this to
// drive HUD toasts and to mark quests dirty for network replication.
enum class QuestEvent {
  kStarted,
  kStopped,
  kStageChanged,
  kObjectiveChanged,
  kActiveChanged,
  kReset,
  kApplied,  // overwritten wholesale from a remote snapshot (multiplayer client)
};

// Per-objective view for the HUD / debugger / network snapshot. Text and
// targets come from the parsed QUST definition (empty when no def is loaded).
struct ObjectiveStatus {
  i32 index = 0;
  std::string text;
  bool displayed = false;
  bool completed = false;
};

// A flattened, self-contained view of one quest's live state plus the display
// metadata the HUD and debugger need. Built on demand from QuestState + QuestDef
// so callers never reach into the system's internals.
struct QuestStatus {
  QuestHandle handle = 0;
  std::string editor_id;
  std::string name;       // FULL text, falls back to editor id then handle
  bool running = false;
  bool active = true;     // shows in the journal / HUD when running
  bool complete = false;  // reached a stage flagged "complete quest"
  i32 stage = 0;
  std::string log_entry;  // journal text of the most recently set stage
  u32 revision = 0;       // bumps on every mutation; lets the net layer delta
  std::vector<ObjectiveStatus> objectives;
};

// The engine's single source of truth for quest runtime state, decoupled from
// Papyrus. The Skyrim bindings delegate every quest op here; the HUD, the
// stage debugger, and the network layer read snapshots and (for the debugger
// and multiplayer clients) push mutations back in.
//
// Threading: the system holds no locks. By construction only one thread (the
// Papyrus guest thread) mutates it; other threads marshal their reads and
// writes onto that thread, exactly as the rest of the binding state is used.
class QuestSystem {
 public:
  using Listener = std::function<void(QuestHandle, QuestEvent)>;

  QuestSystem() = default;

  // Display + structure metadata for a quest (parsed from its QUST record).
  // Replaces any existing definition for the same handle.
  void SetDefinition(QuestDef def);
  const QuestDef* Definition(QuestHandle handle) const;

  // ---- Runtime state, mirroring the Papyrus Quest script surface. ----
  i32 GetStage(QuestHandle quest) const;
  // Sets the current stage and marks it done. Returns true the first time a
  // given stage is set (so the caller runs its fragment exactly once), false
  // when re-setting an already-done stage. Implies the quest is running.
  bool SetStage(QuestHandle quest, i32 stage);
  bool GetStageDone(QuestHandle quest, i32 stage) const;

  bool IsRunning(QuestHandle quest) const;
  // Starts the quest running. The caller drives the opening stage (which it
  // selects from the fragment table), since stage logic lives in the bindings.
  void StartQuest(QuestHandle quest);
  void StopQuest(QuestHandle quest);
  void ResetQuest(QuestHandle quest);

  bool IsActive(QuestHandle quest) const;
  void SetActive(QuestHandle quest, bool active);

  void SetObjectiveDisplayed(QuestHandle quest, i32 objective, bool displayed);
  void SetObjectiveCompleted(QuestHandle quest, i32 objective, bool completed);
  bool IsObjectiveDisplayed(QuestHandle quest, i32 objective) const;
  bool IsObjectiveCompleted(QuestHandle quest, i32 objective) const;

  // True once the quest has set a stage that its definition flags as completing
  // the quest. Without a definition this is always false.
  bool IsComplete(QuestHandle quest) const;

  // ---- Snapshots for HUD / debugger / network. ----
  QuestStatus Status(QuestHandle quest) const;
  // Every quest the system has touched, in unspecified order.
  std::vector<QuestStatus> AllStatuses() const;
  // Running quests, for the HUD. include_inactive keeps quests whose journal is
  // hidden (active == false); the HUD omits them by default.
  std::vector<QuestStatus> RunningStatuses(bool include_inactive = false) const;

  // Overwrites one quest's state from a remote authority (multiplayer client).
  // Does not run fragments; fires kApplied so the HUD refreshes. Stage/objective
  // text is resolved locally from the definition, so only the bits travel.
  void ApplyStatus(const QuestStatus& status);

  // Monotonic counter bumped on every mutation; the net layer compares it to
  // decide whether anything needs to go out this tick.
  u32 revision() const { return revision_; }

  // Listeners fire after each mutation completes. Used for HUD toasts and to
  // flag the quest for replication. Cleared only by destruction.
  void AddListener(Listener listener) { listeners_.push_back(std::move(listener)); }

 private:
  struct QuestState {
    bool running = false;
    bool active = true;
    i32 stage = 0;
    bool ever_set_stage = false;
    std::unordered_map<i32, bool> stage_done;
    std::unordered_map<i32, bool> objective_displayed;
    std::unordered_map<i32, bool> objective_completed;
    u32 revision = 0;
  };

  // Non-inserting lookup; returns nullptr for an untouched quest so const
  // getters stay const and do not pollute the map.
  const QuestState* Peek(QuestHandle quest) const;
  QuestState& Mutable(QuestHandle quest) { return states_[quest]; }
  void Notify(QuestHandle quest, QuestEvent event);
  void FillStatus(QuestHandle quest, const QuestState& state, QuestStatus* out) const;

  std::unordered_map<QuestHandle, QuestState> states_;
  std::unordered_map<QuestHandle, QuestDef> defs_;
  std::vector<Listener> listeners_;
  u32 revision_ = 0;
};

}  // namespace rec::quest

#endif  // RECREATION_QUEST_QUEST_SYSTEM_H_
