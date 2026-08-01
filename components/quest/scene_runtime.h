#ifndef RECREATION_QUEST_SCENE_RUNTIME_H_
#define RECREATION_QUEST_SCENE_RUNTIME_H_

#include <base/containers/vector.h>
#include <base/functional/function.h>
#include <base/strings/xstring.h>

#include "components/quest/condition.h"
#include "components/quest/scene_record.h"
#include "core/types.h"

namespace rx::quest {

// Live playback of a SCEN: the phase machine Bethesda's cutscenes are authored
// against. A scene is a list of phases; each phase holds the actions that play
// inside it (a spoken line, an AI package the performer runs, a wait) and a
// completion condition that decides when the phase is over. Phases run in index
// order, one at a time, and the Papyrus fragments hang off the phase boundaries.
//
// This is the timing half, kept pure so it is unit testable: the runtime feeds it
// resolved beats and answers its condition queries, and it says what should be on
// screen. `scene_player.h` was the placeholder for this (a fixed seconds-per-phase
// cadence with no actions); a scene now advances on its own content.

// One playable beat: a scene action with its alias resolved to a performer and
// its payload resolved to something the engine can act on this frame.
struct SceneBeat {
  enum class Kind : u8 { kDialogue, kPackage, kTimer };

  Kind kind = Kind::kDialogue;
  i32 phase = 0;           // SNAM, the phase the beat starts in
  i32 end_phase = 0;       // ENAM, the last phase it spans (a package holds across phases)
  i32 alias = -1;          // scene actor alias index
  u64 actor = 0;           // performer form handle, 0 when the alias filled nothing
  u64 info = 0;            // kDialogue: the INFO that plays; its fragment runs with the line
  u64 package = 0;         // kPackage: the PACK the performer runs while in its window
  f32 seconds = 0;         // kDialogue: how long the line takes; kTimer: the wait
  base::String speaker;    // display name, for the subtitle
  base::String text;       // subtitle text; empty for a beat with no localized line
  i32 look_at_alias = -1;  // HTID head-track target, which is also who to cut to
};

// A scene lowered for playback. Beats are in play order (phase, then the order
// the actions appear in the record, which is the order the game speaks them).
struct ScenePlan {
  u64 scene = 0;
  u64 quest = 0;
  u32 flags = 0;
  base::Vector<i32> phases;                // ascending, every phase the scene declares
  base::Vector<ConditionList> completion;  // parallel to `phases`
  base::Vector<SceneBeat> beats;

  bool empty() const { return phases.empty() && beats.empty(); }
};

// SCEN FNAM flags we act on. "Begin on quest start" is a real scene trigger: the
// game starts those scenes when their quest comes online, with no script call.
constexpr u32 kSceneBeginOnQuestStart = 0x00000001;
constexpr u32 kSceneStopOnQuestEnd = 0x00000002;
constexpr u32 kSceneInterruptible = 0x00000010;

// Resolvers the engine supplies to lower a parsed SceneDef. Kept as callbacks so
// the lowering stays engine-agnostic and testable with mocks.
struct ScenePlanBindings {
  // Scene actor alias index -> performer form handle (0 when unfilled).
  base::Function<u64(i32 alias)> actor;
  // Scene actor alias index -> display name (the quest's alias name).
  base::Function<base::String(i32 alias)> alias_name;
  // A dialogue action's topic spoken by the actor in `alias` (resolved to `speaker`,
  // which is 0 when the alias has no placed reference): which INFO plays, the
  // subtitle, and how long the line lasts (the voice clip's length where there is
  // one). The alias is passed as well as the reference because a scene's cast is
  // often unplaced, and the alias still names the NPC whose voice type files the
  // recording. False when there is nothing to say, which drops the beat.
  base::Function<
      bool(i32 alias, u64 topic, u64 speaker, u64* info, base::String* text, f32* seconds)>
      line;
  // Raw CTDA payloads -> the native condition IR, with form ids resolved.
  base::Function<ConditionList(const base::Vector<SceneRawCondition>&)> conditions;
};

// How long a dialogue beat holds when nothing resolved a duration for it.
constexpr f32 kSceneDefaultLineSeconds = 2.0f;

// Lowers a parsed scene into a plan. Beats whose alias is unfilled are kept with
// actor 0 so the phase still takes its time (the game plays a scene with a missing
// actor as silence rather than skipping ahead), but a dialogue action with no
// resolvable line at all is dropped.
ScenePlan BuildScenePlan(const SceneDef& def, const ScenePlanBindings& bindings);

// What a playing scene tells the engine. Every hook is optional; the runtime only
// needs ConditionsPass answered to gate phases faithfully.
class SceneRuntimeSink {
 public:
  virtual ~SceneRuntimeSink() = default;

  virtual void OnSceneBegin(const ScenePlan& /*plan*/) {}
  // `completed` is false when the scene was stopped before its last phase.
  virtual void OnSceneEnd(const ScenePlan& /*plan*/, bool /*completed*/) {}
  virtual void OnPhaseBegin(const ScenePlan& /*plan*/, i32 /*phase*/) {}
  virtual void OnPhaseEnd(const ScenePlan& /*plan*/, i32 /*phase*/) {}
  // A line starts / stops being spoken: play the voice + subtitle, run the INFO
  // fragment, point the camera at the speaker.
  virtual void OnLineBegin(const ScenePlan& /*plan*/, const SceneBeat& /*beat*/) {}
  virtual void OnLineEnd(const ScenePlan& /*plan*/, const SceneBeat& /*beat*/) {}
  // A package's phase window opens / closes: the performer starts / stops running it.
  virtual void OnPackageBegin(const ScenePlan& /*plan*/, const SceneBeat& /*beat*/) {}
  virtual void OnPackageEnd(const ScenePlan& /*plan*/, const SceneBeat& /*beat*/) {}

  // Whether a phase's completion gate passes. Defaults to true so a sink that
  // does not model conditions plays the scene straight through.
  virtual bool ConditionsPass(const ConditionList& /*conditions*/) { return true; }
  // Whether the line currently on screen is still being spoken. The engine
  // answers with the voice clip's playback state so a phase follows the audio
  // rather than a guessed duration; the default falls back to `seconds`.
  virtual bool LineStillPlaying(const SceneBeat& /*beat*/) { return false; }
};

// Drives one scene. The engine owns one per playing scene.
class SceneRuntime {
 public:
  // Begins `plan` (borrowed; it must outlive playback). Fires OnSceneBegin and
  // enters the first phase. A plan with no phases begins and ends at once.
  void Start(const ScenePlan* plan, SceneRuntimeSink& sink);
  // Ends playback now: closes the line and packages in flight, ends the phase,
  // then the scene. No-op when nothing is playing.
  void Stop(SceneRuntimeSink& sink);
  void Tick(f32 dt, SceneRuntimeSink& sink);

  bool playing() const { return plan_ != nullptr; }
  const ScenePlan* plan() const { return plan_; }
  i32 phase() const;
  // The line on screen, null between lines. The camera and the subtitle read this.
  const SceneBeat* speaking() const;
  f32 line_elapsed() const { return line_time_; }
  f32 elapsed() const { return time_; }

  // Cap on how long one phase may hold waiting for a completion gate that never
  // passes, so a scene gated on gameplay the engine cannot reach still finishes.
  void set_phase_timeout(f32 seconds) { phase_timeout_ = seconds; }

 private:
  void EnterPhase(size_t index, SceneRuntimeSink& sink);
  void LeavePhase(SceneRuntimeSink& sink);
  void StartNextLine(SceneRuntimeSink& sink);
  void EndLine(SceneRuntimeSink& sink);
  void ClosePackages(i32 phase, SceneRuntimeSink& sink);
  void OpenPackages(i32 phase, SceneRuntimeSink& sink);
  void Finish(bool completed, SceneRuntimeSink& sink);

  const ScenePlan* plan_ = nullptr;
  size_t phase_index_ = 0;
  base::Vector<size_t> line_queue_;  // beats left to speak in this phase
  size_t line_cursor_ = 0;
  size_t line_beat_ = kNoBeat;  // beat index of the line on screen
  base::Vector<size_t> active_packages_;
  f32 time_ = 0;        // seconds since the scene began
  f32 line_time_ = 0;   // seconds the current line has been on screen
  f32 phase_time_ = 0;  // seconds spent in the current phase
  f32 phase_wait_ = 0;  // remaining wait from this phase's timer actions
  f32 phase_timeout_ = 30.0f;
  static constexpr size_t kNoBeat = static_cast<size_t>(-1);
};

}  // namespace rx::quest

#endif  // RECREATION_QUEST_SCENE_RUNTIME_H_
