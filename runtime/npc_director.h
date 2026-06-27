#ifndef RECREATION_RUNTIME_NPC_DIRECTOR_H_
#define RECREATION_RUNTIME_NPC_DIRECTOR_H_

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "core/math.h"
#include "ecs/world.h"
#include "engine_context.h"
#include "quest/scene.h"
#include "world/combat.h"
#include "world/components.h"
#include "world/navgrid.h"

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
  // Ambient sandbox for the streamed NPCs near the player: idle-wander within a
  // small radius of where each was placed, and turn to face a close player, so
  // the world is not full of statues. Host/single-player authoritative; the
  // motion streams to clients via actor sync, like followers. Cheap: only NPCs
  // near the player are simulated and the per-frame steering work is capped.
  void UpdateAmbient(f32 dt);
  // Melee combat: enrolled attackers close on their target and swing on a
  // cooldown; connecting hits remove the target's health (applied on the guest
  // thread, where actor values + the OnDeath path live). EnterCombat/LeaveCombat
  // and OnActorDied are fed from the guest-thread combat queue each frame.
  // Host/single-player authoritative, like the other NPC motion.
  void EnterCombat(u64 attacker, u64 target);
  void LeaveCombat(u64 attacker);
  void OnActorDied(u64 actor);
  void UpdateCombat(f32 dt);
  int combatant_count() const { return static_cast<int>(combat_.size()); }
  // The side the player fights on (a world::CombatTeam id), so hostile soldiers
  // target the player; 0 (default) leaves the player out of the auto-aggression.
  void set_player_team(i32 team) { player_team_ = team; }
  // A player melee swing from `pos` facing `yaw`: the nearest living NPC in the
  // forward arc and reach takes a heavy hit (damage applied on the guest thread,
  // so it flows through the normal OnDeath path). Returns true if it connected.
  bool PlayerMeleeStrike(const Vec3& pos, f32 yaw);
  // A reachable waypoint from `from` toward `goal` that rounds interior walls
  // (grid A* over a downward-ray floor map). Lets the engine route the walking
  // player through the keep instead of pressing it straight into geometry.
  Vec3 PathToward(const Vec3& from, const Vec3& goal);
  void Mq101DemoTick(f32 dt);
  void Mq101SceneTick(f32 dt);

  // Load-time arming from the quest director's REC_MQ101_* hooks.
  void ArmMq101Demo(u64 quest_handle);
  void ArmMq101Scene(u64 quest_handle);

  // REC_CW_BATTLE verification harness: enlist the streamed NPCs near the player
  // into two armies (CombatTeam) and let the combat driver fight it out. Exercises
  // the combat path (closing, swinging, dying) on real actors, independent of the
  // Civil War Papyrus. Not the quest itself.
  void ArmCwBattle() { cw_battle_pending_ = true; }
  void CwBattleTick(f32 dt);
  // Live army strength for the battle HUD: alive count per team and fallen total.
  // Returns false until a battle is running.
  bool BattleStrength(int* team_a, int* team_b, int* fallen) const;

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
  // Resolves a form handle to its live world entity and transform (the player,
  // streamed NPCs, and quest-spawned refs). False if it has no body in the world
  // yet (e.g. an actor in an unstreamed cell), in which case the combatant stays
  // enrolled and engages once it streams in.
  bool ResolveCombatant(u64 handle, ecs::Entity* entity, world::Transform** transform);
  // Auto-aggression: enrolls idle teamed actors against the nearest hostile.
  void AcquireTargets();
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

  world::NavGrid navgrid_;  // cached interior routing for followers/guides/player
  base::UnorderedMap<u64, i32> followers_;
  base::UnorderedMap<u64, Vec3> guides_;
  // Ambient-wander state per nearby NPC (keyed by its form handle), pruned each
  // frame to the NPCs still near the player. xorshift jitter is local sandbox
  // noise (host-only), never replicated, so it needs no determinism guarantee.
  struct AmbientState {
    Vec3 home{};
    Vec3 goal{};
    f32 idle_timer = 0;
    bool walking = false;
  };
  base::UnorderedMap<u64, AmbientState> ambient_;
  // Per-attacker melee state: the target it fights and its swing cooldown. Fed by
  // the guest combat queue (StartCombat/StopCombat/death). The player can be a
  // target (NPCs swing at it) but is never an auto-attacker (it fights by input).
  struct CombatState {
    u64 target = 0;
    f32 swing_timer = 0;
    u32 rng = 0x9e3779b9u;
  };
  base::UnorderedMap<u64, CombatState> combat_;
  world::CombatParams combat_params_;
  f32 combat_acquire_timer_ = 0;  // throttles the auto-aggression target scan
  i32 player_team_ = 0;           // 0 = player not in the auto-aggression
  bool cw_battle_pending_ = false;
  bool cw_battle_active_ = false;
  f32 cw_battle_log_timer_ = 0;
  u64 ambient_rng_ = 0x243f6a8885a308d3ull;
  f32 AmbientRand(f32 lo, f32 hi);
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
