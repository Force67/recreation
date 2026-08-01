#ifndef RECREATION_RUNTIME_CUTSCENE_DIRECTOR_H_
#define RECREATION_RUNTIME_CUTSCENE_DIRECTOR_H_

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "bethesda/form_id.h"
#include "core/math.h"
#include "engine_context.h"
#include "quest/quest_def.h"
#include "quest/scene_runtime.h"
#include "quest_state_cache.h"
#include "trailer.h"
#include "world/cine_camera.h"

namespace rx {

class ActorSystem;
class AiPackageDirector;
class InteractionSystem;
class NpcDirector;

// Plays the games' cutscenes, from their own records.
//
// A Bethesda cutscene is a SCEN: a cast of quest aliases, a list of phases, and
// per-phase actions (a spoken line, an AI package to run, a wait). None of it
// carries camera data or timing: the game speaks each line for as long as its voice
// clip lasts, frames whoever is talking procedurally, and moves actors by handing
// them packages. So this director is the whole of it, for every scene in the game:
//
//   * scenes are indexed by owning quest and started the way the game starts them,
//     by Scene.Start from a quest fragment or by the "begin on quest start" flag;
//   * a scene's dialogue resolves to the real INFO records, their subtitle text and
//     their voice files, so the pacing is the recording's pacing;
//   * the camera is the shared dialogue camera (world/cine_camera.h), cutting
//     between speaker and listener as the conversation moves;
//   * scene packages go to the AI package driver, the same one that runs an actor's
//     ordinary alias packages, so actors walk to their marks.
//
// Nothing here knows about a specific quest.
class CutsceneDirector {
 public:
  CutsceneDirector(EngineContext& ctx, ActorSystem* actors, NpcDirector* npc,
                   AiPackageDirector* packages);
  ~CutsceneDirector();
  void set_interaction(InteractionSystem* interaction) { interaction_ = interaction; }

  // Indexes every SCEN in the load order by its owning quest. Records only.
  void IndexScenes();
  int scene_count() const { return static_cast<int>(scenes_.size()); }

  // A quest coming online starts the scenes it owns that are flagged to begin with
  // it (the Helgen cart ride is one of those).
  void OnQuestStarted(u64 quest);

  // The quest an explicit cutscene run is watching (RX_CUTSCENE), so the world can
  // boot where its scenes happen instead of at the default start cell.
  void set_armed_quest(u64 quest) { armed_quest_ = quest; }
  // The quest a name refers to, among the quests that own scenes. The quest list the
  // script host keeps only covers quests with Papyrus attached, and most of the
  // game's conversation scenes hang off script-less dialogue quests.
  u64 FindQuestByEditorId(const std::string& editor_id);

  // Where the armed quest's first scene plays, from its cast's authored placements.
  // False when nothing is armed or none of the cast is placed in an exterior.
  bool ArmedSceneLocation(Vec3* pos);

  bool StartScene(u64 scene);
  void StopScene(u64 scene);
  bool IsPlaying(u64 scene) const;
  int playing_count() const { return static_cast<int>(playing_.size()); }

  // Main thread, once per sim tick: drains the Scene.Start calls the guest made,
  // advances every playing scene, and drives the voices, captions and camera.
  void Tick(f32 dt, const QuestStateCache& quests);

  // True while a scene is framing itself, in which case the engine hands over the
  // camera and holds the player's input (a cutscene the player is watching).
  bool owns_view() const { return owns_view_; }
  // Hands the view back to the player for the rest of the scene (their escape key
  // out of a cutscene camera they do not want).
  void ReleaseView() { view_released_ = true; }
  bool CameraOverride(Vec3* eye, Vec3* target) const;
  // Caption / letterbox chrome for this frame; `active` is false when idle.
  const TrailerOverlay& overlay() const { return overlay_; }

  // Headless verification: for every quest whose editor id starts with `prefix`,
  // lower each of its scenes and report what resolved (cast, phases, spoken lines,
  // voice clips, running time). This is the coverage check behind the cutscene
  // table: it reads the same data the live director plays.
  void ReportQuestCutscenes(const std::string& prefix);

  // Lines spoken so far and the last one, for the debugger and for verification.
  u32 lines_spoken() const { return lines_spoken_; }
  const std::string& last_line() const { return last_line_; }
  std::vector<std::string> Report() const;

 private:
  // One scene, with the plan its runtime is walking. Held by pointer because the
  // runtime borrows the plan.
  struct Playing {
    u64 scene = 0;
    u64 quest = 0;
    u16 plugin = 0;
    std::string editor_id;
    quest::ScenePlan plan;
    quest::SceneRuntime runtime;
    u32 voice = 0;      // audio voice id of the line in flight, 0 when silent
    f32 voice_hold = 0;  // seconds of clip left, so the beat can follow the audio
    u64 speaker = 0;     // performer of the line on screen
    u64 addressee = 0;   // who they are speaking to, for the reverse angle
    std::string caption_speaker;
    std::string caption;
    std::vector<u64> cast;   // performer refs, for the enable pass below
    f32 enable_timer = 0;    // retries enabling the cast as it streams in
  };

  // An indexed scene: enough to start it without re-scanning the records.
  struct SceneEntry {
    u64 scene = 0;
    u64 quest = 0;
    u16 plugin = 0;
    u32 flags = 0;
    std::string editor_id;
    bool scripts_attached = false;
  };

  // A resolved voice line: the clip and how long it runs. `voice_type` is the
  // directory it was looked for in, so a miss can say whether the speaker had no
  // voice type at all or the file simply was not there.
  struct VoiceLine {
    std::string path;
    f32 seconds = 0;
    bool had_voice_type = false;
  };

  class Sink;

  quest::ScenePlan BuildPlan(const SceneEntry& entry, const quest::SceneDef& def);
  // Attaches the scene's SF_ script and registers its fragments, so the phase
  // fragments (which are what advance the journal) can run. Main thread only.
  void EnsureSceneScripts(SceneEntry& entry);
  // Switches on any of a scene's cast that is still disabled, so the scene has the
  // actors it was authored around.
  void EnableCast(u64 quest, const std::string& editor_id, const std::vector<u64>& cast);
  // Stands a scene's cast on the ground. A quest stages its actors by teleporting
  // them onto markers authored against the game's own terrain; where our heightfield
  // sits a little higher they end up buried, which reads as an empty cutscene.
  void GroundCast(const std::vector<u64>& cast);
  const quest::QuestDef* QuestDefinition(u64 quest);
  // Whether the player is one of the scene's performers, which decides both who a
  // line is addressed to and whether this is the player's own cutscene.
  bool PlayerInCast(const quest::ScenePlan& plan) const;
  // The reference an alias fills, from the records: a forced reference, or the
  // placement of the unique actor it names.
  u64 AliasReference(const quest::QuestDef& def, i32 alias, u16 plugin) const;
  std::string AliasName(const quest::QuestDef& def, i32 alias) const;
  // The line a speaker says under a topic: the INFO, its subtitle and its length.
  bool ResolveLine(const SceneEntry& entry, i32 alias, u64 topic, u64 speaker, u64* info,
                   std::string* text, f32* seconds);
  VoiceLine ResolveVoice(const SceneEntry& entry, i32 alias, u64 topic, u64 topic_quest,
                         u64 speaker, u64 info, int response_index, const std::string& text);
  // The voice type a scene actor's lines are filed under: the placed reference's
  // base NPC when it has one, else the NPC the alias names directly.
  std::string VoiceTypeFor(const SceneEntry& entry, i32 alias, u64 speaker);
  // Head position of a performer, which is what the camera frames. Falls back to
  // the body position, then to the authored placement for an actor not streamed in.
  bool HeadOf(u64 handle, Vec3* out) const;
  // Whether a performer has a body in the world right now, as opposed to only an
  // authored placement in an unstreamed cell.
  bool LiveActor(u64 handle) const;
  // Human-readable home of a scene: the interior cell its cast stands in, or the
  // exterior cell grid. Empty when none of the cast is placed.
  std::string SceneLocation(const SceneEntry& entry, const quest::SceneDef& def);
  void DriveCamera(f32 dt);
  void UpdateOverlay(f32 dt);
  void RunSceneCue(u64 scene, int phase, bool on_begin, bool scene_edge);

  EngineContext& ctx_;
  ActorSystem* actors_;
  NpcDirector* npc_;
  AiPackageDirector* packages_;
  InteractionSystem* interaction_ = nullptr;

  std::vector<SceneEntry> scenes_;
  std::unordered_map<u64, size_t> scene_index_;         // scene handle -> scenes_
  std::unordered_map<u64, std::vector<size_t>> by_quest_;
  std::unordered_map<u64, quest::QuestDef> quest_defs_;  // parsed on demand
  std::unordered_map<u64, VoiceLine> voice_cache_;       // INFO handle -> clip
  std::vector<std::unique_ptr<Playing>> playing_;
  std::vector<u64> quests_seen_running_;  // for the begin-on-quest-start edge
  u64 armed_quest_ = 0;
  i32 armed_stage_ = -1;

  const QuestStateCache* quests_ = nullptr;  // valid during Tick
  world::ShotDirector shots_;
  world::CineFraming framing_{};
  bool framing_valid_ = false;
  bool owns_view_ = false;
  bool view_released_ = false;
  f32 caption_fade_ = 0;
  TrailerOverlay overlay_;
  u32 lines_spoken_ = 0;
  std::string last_line_;
};

}  // namespace rx

#endif  // RECREATION_RUNTIME_CUTSCENE_DIRECTOR_H_
