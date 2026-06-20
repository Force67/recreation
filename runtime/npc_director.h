#ifndef RECREATION_RUNTIME_NPC_DIRECTOR_H_
#define RECREATION_RUNTIME_NPC_DIRECTOR_H_

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "core/math.h"
#include "ecs/world.h"
#include "engine_context.h"
#include "quest/scene.h"

namespace rec {

class ActorSystem;
class InteractionSystem;
class QuestDirector;

// The interactive-NPC layer: follower steering behind the player, scene guides
// steered toward fixed targets, obstacle avoidance + grid pathfinding, and the
// two scripted MQ101 playthroughs (the breadcrumb demo and the escort scene).
// Host authoritative; the motion streams to clients via actor sync.
class NpcDirector {
 public:
  NpcDirector(EngineContext& ctx, ActorSystem* actors);
  void set_siblings(InteractionSystem* interaction, QuestDirector* quest) {
    interaction_ = interaction;
    quest_ = quest;
  }

  void SetFollower(u64 npc, bool follow);
  int follower_count() const { return static_cast<int>(followers_.size()); }
  bool is_follower(u64 npc) const { return followers_.find(npc) != nullptr; }
  // True while a scripted MQ101 playthrough is driving the auto-walk goal, so
  // the engine leaves the guided goal alone and only steers toward the tracked
  // objective itself when no playthrough owns it.
  bool guiding() const {
    return mq101_demo_pending_ || mq101_scene_pending_ || mq101_scene_active_;
  }

  void UpdateFollowers(f32 dt);
  void UpdateGuides(f32 dt);
  void Mq101DemoTick(f32 dt);
  void Mq101SceneTick(f32 dt);

  // Load-time arming from the quest director's REC_MQ101_* hooks.
  void ArmMq101Demo(u64 quest_handle);
  void ArmMq101Scene(u64 quest_handle);

 private:
  // SceneSink over the running engine: a scene guides NPCs, runs INFO fragments,
  // advances quest stages, and answers proximity queries.
  struct Mq101Sink : public quest::SceneSink {
    NpcDirector* d = nullptr;
    void GuideTo(u64 actor, const float pos[3]) override;
    void SayInfo(u64 actor, u64 info) override;
    void SetStage(u64 quest, i32 stage) override;
    bool ActorAt(u64 actor, const float pos[3], float radius) override;
    bool PlayerNear(const float pos[3], float radius) override;
  };

  void AvoidObstacles(const float self_pos[3], const float goal_dir[3], float out_dir[3]);
  bool StepNpcSteering(ecs::Entity actor, const float goal[3], float pos[3], float rot[4],
                       float speed, float arrive_radius, float stop_radius, f32 dt);
  Vec3 NavigateTo(const Vec3& from, const Vec3& goal);
  // Floor height under (x, z), found by a short downward ray; returns y_hint when
  // nothing is beneath (a ledge / unloaded collision) so the caller holds height.
  f32 GroundY(f32 x, f32 z, f32 y_hint) const;
  bool StartMq101Scene();

  EngineContext& ctx_;
  ActorSystem* actors_;
  InteractionSystem* interaction_ = nullptr;
  QuestDirector* quest_ = nullptr;
  ecs::World& world_;
  physics::PhysicsWorld& physics_;

  base::UnorderedMap<u64, i32> followers_;
  base::UnorderedMap<u64, Vec3> guides_;
  bool mq101_demo_pending_ = false;
  u64 mq101_demo_quest_ = 0;
  base::Vector<i32> mq101_demo_stages_;
  size_t mq101_demo_next_ = 0;
  f32 mq101_demo_wait_ = 0;
  quest::Scene mq101_scene_;
  quest::SceneRunner scene_runner_;
  Mq101Sink scene_sink_;
  u64 mq101_scene_quest_ = 0;
  bool mq101_scene_pending_ = false;
  bool mq101_scene_active_ = false;
  f32 mq101_scene_stuck_time_ = 0;  // seconds the current beat has not progressed
};

}  // namespace rec

#endif  // RECREATION_RUNTIME_NPC_DIRECTOR_H_
