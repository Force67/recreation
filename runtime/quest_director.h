#ifndef RECREATION_RUNTIME_QUEST_DIRECTOR_H_
#define RECREATION_RUNTIME_QUEST_DIRECTOR_H_

#include <string>
#include <vector>

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "core/math.h"
#include "engine_context.h"
#include "quest/quest_def.h"
#include "quest/quest_system.h"

namespace rec {

class ActorSystem;
class NpcDirector;
class InteractionSystem;

// A world waypoint for one quest objective: where it sends the player and the
// stage reaching it advances to. Shared with the npc director, which authors
// breadcrumb waypoints for the MQ101 demo.
struct QuestMarker {
  u64 quest = 0;
  i32 objective = 0;
  i32 advance_stage = -1;  // <0: display-only, never triggers
  Vec3 pos{};
  f32 radius = 2.5f;
  bool fired = false;       // a trigger fires once; cleared by re-authoring
  bool always_arm = false;  // armed regardless of which objective is current
};

// Drives everything quest-facing: instantiates the attached Papyrus quest
// scripts, refreshes the debug overlay + native-call trace, pushes the HUD
// tracker and journal, arms objective waypoints, and runs the headless report
// modes. Owns the quest record list, panel snapshot, markers and objective
// compass targets.
class QuestDirector {
 public:
  QuestDirector(EngineContext& ctx, ActorSystem* actors);
  void set_siblings(NpcDirector* npc, InteractionSystem* interaction) {
    npc_ = npc;
    interaction_ = interaction;
  }

  // Instantiates the attached quest scripts and applies the REC_START_QUEST /
  // REC_MQ101_* / REC_JOURNAL load-time hooks.
  void AttachQuestScripts();
  void RefreshQuestPanel(f32 dt);
  void RefreshNativeTrace(f32 dt);
  // Live overlay snapshots the debug UI reads when building the F3 / F2 windows.
  QuestPanel* quest_panel() { return &quest_panel_; }
  NativeTracePanel* native_trace_panel() { return &native_trace_panel_; }

  // Headless debug aids (REC_QUEST_REPORT / REC_DIALOGUE_REPORT).
  void ReportQuestToCompletion(const std::string& edid);
  void ReportDialogue(const std::string& edid);

  // Editor-id -> quest handle (0 if unknown); used by the npc director's scene.
  u64 FindQuestHandle(const std::string& edid) const;
  base::Vector<QuestMarker>& markers() { return quest_markers_; }
  void Select(u64 handle) { quest_panel_.selected = handle; }

  // The host's replicated objective marker, mirrored onto this client's compass.
  void SetRemoteMarker(bool active, const Vec3& pos) {
    remote_marker_active_ = active;
    remote_marker_pos_ = pos;
  }

  // Player journal controls (J toggles it; number keys pin a quest to track).
  bool journal_open() const { return journal_open_; }
  void ToggleJournal() { journal_open_ = !journal_open_; }
  void PinJournalSlot(int i);

 private:
  void UpdateQuestHud(const std::vector<quest::QuestStatus>& running);
  void UpdateObjectiveMarkers(const std::vector<quest::QuestStatus>& running);
  void DriveObjectiveMarkerHud(bool active, const Vec3& pos);
  void IndexObjectiveTargets(const quest::QuestDef& def, u16 plugin);
  bool ObjectiveTargetFor(u64 quest, i32 objective, Vec3* out) const;
  bool RefWorldPosition(bethesda::GlobalFormId ref, Vec3* out) const;

  EngineContext& ctx_;
  ActorSystem* actors_;
  NpcDirector* npc_ = nullptr;
  InteractionSystem* interaction_ = nullptr;
  const EngineConfig& config_;
  ecs::World& world_;
  bethesda::RecordStore& records_;
  bethesda::StringTable& strings_;
  dialogue::DialogueDb& dialogue_;
  FlyCamera& camera_;
  DebugUi& debug_ui_;
  GameUi& game_ui_;

  base::Vector<std::pair<u64, std::string>> quest_records_;
  QuestPanel quest_panel_;
  f32 quest_ui_timer_ = 0;
  u64 hud_tracked_quest_ = 0;
  u32 hud_tracked_revision_ = 0;
  bool journal_open_ = false;
  u64 pinned_quest_ = 0;
  base::Vector<u64> journal_handles_;
  base::Vector<QuestMarker> quest_markers_;
  // Objective index -> world position of its forced-reference alias, with the
  // space that position lives in. An interior ref's position is in its own cell
  // space, so a compass bearing to it is only meaningful while the player is in
  // that same space (handled by ObjectiveTargetFor).
  struct ObjTarget {
    Vec3 pos;
    bool interior = false;
  };
  base::UnorderedMap<u64, ObjTarget> objective_targets_;
  bool sent_marker_active_ = false;
  Vec3 sent_marker_pos_{};
  u64 sent_marker_quest_ = 0;
  bool remote_marker_active_ = false;
  Vec3 remote_marker_pos_{};
  NativeTracePanel native_trace_panel_;
  f32 trace_ui_timer_ = 0;
  bool native_trace_on_ = false;
};

}  // namespace rec

#endif  // RECREATION_RUNTIME_QUEST_DIRECTOR_H_
