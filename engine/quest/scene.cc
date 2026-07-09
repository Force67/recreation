#include "quest/scene.h"

namespace rx::quest {

void SceneRunner::Reset(const Scene* scene) {
  scene_ = scene;
  index_ = 0;
  started_ = false;
  elapsed_ = 0;
}

bool SceneRunner::running() const {
  return scene_ != nullptr && index_ < scene_->actions.size();
}

bool SceneRunner::Tick(SceneSink& sink, float dt) {
  if (!scene_ || index_ >= scene_->actions.size()) return false;

  const SceneAction& a = scene_->actions[index_];

  // Issue the action's command exactly once, on first visit. kWait and
  // kWaitPlayerNear are pure holds, so they issue nothing.
  if (!started_) {
    started_ = true;
    elapsed_ = 0;
    switch (a.kind) {
      case SceneAction::Kind::kGuideTo:
        sink.GuideTo(a.actor, a.pos);
        break;
      case SceneAction::Kind::kSayInfo:
        sink.SayInfo(a.actor, a.info);
        break;
      case SceneAction::Kind::kSetStage:
        sink.SetStage(a.quest, a.stage);
        break;
      case SceneAction::Kind::kWait:
      case SceneAction::Kind::kWaitPlayerNear:
        break;
    }
  }

  elapsed_ += dt;

  bool done = false;
  switch (a.kind) {
    case SceneAction::Kind::kGuideTo:
      done = sink.ActorAt(a.actor, a.pos, a.radius);
      break;
    case SceneAction::Kind::kSayInfo:
    case SceneAction::Kind::kSetStage:
      done = true;
      break;
    case SceneAction::Kind::kWait:
      done = elapsed_ >= a.seconds;
      break;
    case SceneAction::Kind::kWaitPlayerNear:
      done = sink.PlayerNear(a.pos, a.radius);
      break;
  }

  if (done) {
    ++index_;
    started_ = false;
  }
  return index_ < scene_->actions.size();
}

}  // namespace rx::quest
