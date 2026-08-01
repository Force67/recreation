#ifndef RECREATION_QUEST_SCENE_H_
#define RECREATION_QUEST_SCENE_H_

#include <vector>

#include "core/types.h"

namespace rx::quest {

// One beat of a scene. Fields are read per kind:
//   kGuideTo        -> actor walks to pos; done when the actor reaches it (radius)
//   kSayInfo        -> actor runs INFO `info` (its dialogue fragment); done at once
//   kSetStage       -> quest advances to stage; done at once
//   kWait           -> hold for `seconds`
//   kWaitPlayerNear -> hold until the player is within radius of pos
struct SceneAction {
  enum class Kind : u8 { kGuideTo, kSayInfo, kSetStage, kWait, kWaitPlayerNear };
  Kind kind = Kind::kWait;
  u64 actor = 0;     // form handle for kGuideTo / kSayInfo
  u64 info = 0;      // INFO handle for kSayInfo
  u64 quest = 0;     // quest handle for kSetStage
  i32 stage = 0;     // stage for kSetStage
  float pos[3] = {0, 0, 0};  // target for kGuideTo / kWaitPlayerNear
  float radius = 2.5f;       // arrival radius for kGuideTo / kWaitPlayerNear
  float seconds = 0;         // duration for kWait
};

struct Scene {
  std::vector<SceneAction> actions;  // run in order, one at a time
};

// Side effects + queries a running scene needs. The engine implements this over
// its NPC steering and quest system; tests record the calls and script the
// query answers. Mirrors QuestActionSink.
class SceneSink {
 public:
  virtual ~SceneSink() = default;
  virtual void GuideTo(u64 /*actor*/, const float /*pos*/[3]) {}  // start the actor walking there
  virtual void SayInfo(u64 /*actor*/, u64 /*info*/) {}           // run the actor's INFO fragment
  virtual void SetStage(u64 /*quest*/, i32 /*stage*/) {}         // advance the journal
  virtual bool ActorAt(u64 /*actor*/, const float /*pos*/[3], float /*radius*/) { return true; }
  virtual bool PlayerNear(const float /*pos*/[3], float /*radius*/) { return false; }
};

// Drives one Scene through its actions via the sink. Each action's command is
// issued once (on entry); the action completes on its own signal (arrival /
// immediate / timer / player proximity), then the runner advances. Holds no
// engine state, so it is unit testable with a recording sink.
class SceneRunner {
 public:
  SceneRunner() = default;
  explicit SceneRunner(const Scene* scene) : scene_(scene) {}
  void Reset(const Scene* scene);                  // restart on a (new) scene
  // Advances the scene by dt seconds. Returns true while running, false when
  // every action has completed (or there is no scene).
  bool Tick(SceneSink& sink, float dt);
  bool running() const;                            // a scene is set and not finished
  size_t current_action() const { return index_; }
 private:
  const Scene* scene_ = nullptr;
  size_t index_ = 0;
  bool started_ = false;  // current action's command has been issued
  float elapsed_ = 0;     // accumulates for kWait
};

}  // namespace rx::quest

#endif  // RECREATION_QUEST_SCENE_H_
