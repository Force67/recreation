#ifndef RECREATION_RUNTIME_QUEST_STATE_CACHE_H_
#define RECREATION_RUNTIME_QUEST_STATE_CACHE_H_

#include <functional>
#include <unordered_map>
#include <vector>

#include "core/math.h"
#include "core/types.h"
#include "quest/condition.h"

namespace rx {

// A main-thread mirror of live quest state, refreshed from the guest thread a few
// times a second alongside the quest HUD snapshot.
//
// Bethesda gates almost everything on quest stage: which AI package an actor runs,
// when a scene phase is over, which dialogue is available. Those gates have to be
// evaluated by the systems that own the world (the AI package driver, the cutscene
// director), and those live on the main thread while QuestSystem is guest-thread
// only. Mirroring the handful of numbers they read is cheaper and far simpler than
// marshalling every evaluation.
class QuestStateCache {
 public:
  struct Entry {
    i32 stage = 0;
    bool running = false;
    bool complete = false;
    std::vector<i32> done;  // stages that have been set, ascending
  };

  void Set(u64 quest, Entry entry) { quests_[quest] = std::move(entry); }
  void Clear() { quests_.clear(); }

  i32 Stage(u64 quest) const {
    auto it = quests_.find(quest);
    return it == quests_.end() ? 0 : it->second.stage;
  }
  bool Running(u64 quest) const {
    auto it = quests_.find(quest);
    return it != quests_.end() && it->second.running;
  }
  bool Complete(u64 quest) const {
    auto it = quests_.find(quest);
    return it != quests_.end() && it->second.complete;
  }
  bool StageDone(u64 quest, i32 stage) const {
    auto it = quests_.find(quest);
    if (it == quests_.end()) return false;
    for (i32 s : it->second.done)
      if (s == stage) return true;
    return false;
  }
  const std::unordered_map<u64, Entry>& entries() const { return quests_; }

 private:
  std::unordered_map<u64, Entry> quests_;
};

// Evaluates condition lists on the main thread against the quest mirror and the
// live world. Stage functions come from the cache; distances are measured against
// real positions, which the caller resolves by form handle. Anything else falls
// through to 0, the conservative answer for the threshold tests these gates use.
class WorldConditionContext : public quest::ConditionContext {
 public:
  using PositionFn = std::function<bool(u64 handle, Vec3* out)>;

  WorldConditionContext(const QuestStateCache& quests, PositionFn position)
      : quests_(quests), position_(std::move(position)) {}

  float GetStage(u64 quest) const override { return static_cast<float>(quests_.Stage(quest)); }
  float GetStageDone(u64 quest, u64 stage) const override {
    return quests_.StageDone(quest, static_cast<i32>(stage)) ? 1.0f : 0.0f;
  }
  float GetDistance(quest::RunOn, u64 reference, u64 target) const override {
    Vec3 a, b;
    if (!position_ || !position_(reference, &a) || !position_(target, &b)) return 0.0f;
    // Game units, which is what the authored comparison values are in.
    constexpr float kMetersToUnits = 1.0f / 0.01428f;
    return Length(b - a) * kMetersToUnits;
  }

 private:
  const QuestStateCache& quests_;
  PositionFn position_;
};

}  // namespace rx

#endif  // RECREATION_RUNTIME_QUEST_STATE_CACHE_H_
