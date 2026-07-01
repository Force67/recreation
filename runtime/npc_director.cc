#include "npc_director.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <base/option.h>

#include "actor_system.h"
#include "core/log.h"
#include "core/math.h"
#include "engine_internal.h"
#include "interaction_system.h"
#include "quest_director.h"
#include "script/papyrus/value.h"
#include "world/components.h"
#include "world/npc_ai.h"
#include "world/steering_avoidance.h"

namespace rec {

static base::Option<float> CwBattleDelay{"cw.battle.delay", 0.f, "REC_CW_BATTLE_DELAY"};

NpcDirector::NpcDirector(EngineContext& ctx, ActorSystem* actors)
    : ctx_(ctx), actors_(actors), world_(*ctx.world), physics_(*ctx.physics) {}

void NpcDirector::ArmMq101Demo(u64 quest_handle) {
  mq101_demo_quest_ = quest_handle;
  // Curated gameplay beats of Unbound; each stage exists in the QUST and runs a
  // fragment that advances the journal (160 "Make your way to the Keep", on
  // through entering and escaping the keep, to completion 900).
  mq101_demo_stages_.clear();
  for (i32 s : {160, 300, 500, 700, 900}) mq101_demo_stages_.push_back(s);
  mq101_demo_next_ = 1;  // SetStage(stages[0]) now; waypoints drive stages[1..]
  const i32 first = mq101_demo_stages_[0];
  auto* binds = ctx_.bindings;
  ctx_.scripts->guest().Submit([binds, quest_handle, first](script::papyrus::VirtualMachine&) {
    script::papyrus::ObjectRef ref{quest_handle};
    binds->StartQuest(ref);
    binds->SetStage(ref, first);
  });
  mq101_demo_pending_ = true;
}

void NpcDirector::SetFollower(u64 npc, bool follow) {
  if (follow) {
    if (!followers_.find(npc)) followers_.insert(npc, static_cast<i32>(followers_.size()));
  } else {
    followers_.erase(npc);
  }
}

void NpcDirector::AvoidObstacles(const float self_pos[3], const float goal_dir[3], float out_dir[3]) {
  // Without physics (e.g. a stub build) there is nothing to raycast against, so
  // steer straight at the goal.
  if (!physics_.initialized()) {
    out_dir[0] = goal_dir[0];
    out_dir[1] = 0;
    out_dir[2] = goal_dir[2];
    return;
  }
  // Fan candidate directions around the goal and raycast each from chest height
  // for the open distance ahead, then let the context steerer pick a clear,
  // still-goal-ish way around whatever is in front.
  constexpr int kFan = 9;
  constexpr float kAngles[kFan] = {0, 20, -20, 40, -40, 60, -60, 80, -80};  // degrees
  constexpr float kLookAhead = 3.0f;
  float candidates[kFan * 3];
  float clearances[kFan];
  const Vec3 origin{self_pos[0], self_pos[1] + 1.0f, self_pos[2]};
  for (int i = 0; i < kFan; ++i) {
    const float a = kAngles[i] * 0.0174533f;
    const float c = std::cos(a), s = std::sin(a);
    const float dx = goal_dir[0] * c + goal_dir[2] * s;  // rotate goal_dir about +Y
    const float dz = -goal_dir[0] * s + goal_dir[2] * c;
    candidates[i * 3 + 0] = dx;
    candidates[i * 3 + 1] = 0;
    candidates[i * 3 + 2] = dz;
    physics::PhysicsWorld::RayHit hit;
    clearances[i] = physics_.Raycast(origin, Vec3{dx, 0, dz}, kLookAhead, &hit) ? hit.distance
                                                                               : kLookAhead;
  }
  world::SteerAroundObstacles(goal_dir, candidates, clearances, kFan, 1.5f, out_dir);
}

f32 NpcDirector::GroundY(f32 x, f32 z, f32 y_hint) const {
  // Prefer the streamed terrain heightmap: it exists wherever the cell is loaded
  // (the same source the player spawn snaps to), whereas the physics collision
  // streams in a few frames later and, until it does, leaves battle actors
  // floating at eye height on open ground. The physics cast still refines onto
  // objects/roads stacked above the terrain when collision is present.
  f32 h = 0;
  if (ctx_.streamer && ctx_.streamer->GroundHeight(x, z, &h)) return h;
  if (!physics_.initialized()) return y_hint;
  // Cast down from just above the current height so a slightly embedded NPC still
  // finds the floor; cap the reach so a gap or ledge leaves the NPC where it was
  // rather than snapping it to a distant floor below.
  physics::PhysicsWorld::RayHit hit;
  if (physics_.Raycast(Vec3{x, y_hint + 1.5f, z}, Vec3{0, -1, 0}, 4.0f, &hit)) return hit.position.y;
  return y_hint;
}

bool NpcDirector::StepNpcSteering(ecs::Entity actor, const float goal[3], float pos[3], float rot[4],
                             float speed, float arrive_radius, float stop_radius, f32 dt) {
  const world::SteerParams params{speed, arrive_radius, stop_radius};
  const float self_pos[3] = {pos[0], pos[1], pos[2]};
  const world::SteerOutput out = world::SteerToward(self_pos, goal, params);
  float move_yaw = out.yaw;
  if (!out.arrived) {
    // Deflect the straight-line steer around nearby obstacles, then move along
    // the chosen direction at the arrival-adjusted speed.
    const float spd = std::sqrt(out.velocity[0] * out.velocity[0] +
                                       out.velocity[2] * out.velocity[2]);
    const float gx = goal[0] - self_pos[0], gz = goal[2] - self_pos[2];
    const float gl = std::sqrt(gx * gx + gz * gz);
    const float goal_dir[3] = {gl > 1e-4f ? gx / gl : 0.0f, 0.0f, gl > 1e-4f ? gz / gl : 0.0f};
    float steer_dir[3];
    AvoidObstacles(self_pos, goal_dir, steer_dir);
    pos[0] += steer_dir[0] * spd * dt;
    pos[2] += steer_dir[2] * spd * dt;
    move_yaw = std::atan2(steer_dir[0], steer_dir[2]);
    const float h = move_yaw * 0.5f;
    rot[0] = 0;
    rot[1] = std::sin(h);
    rot[2] = 0;
    rot[3] = std::cos(h);
  }
  // Keep feet on the floor: ramps and the keep stairs would otherwise leave the
  // NPC floating at its spawn height or sunk through the steps.
  pos[1] = GroundY(pos[0], pos[2], pos[1]);
  // Drive the render actor's gait, if this NPC has a streamed-in instance.
  actors_->SetNpcGait(actor, out.arrived ? 0.0f : speed, !out.arrived, move_yaw);
  return !out.arrived;
}

f32 NpcDirector::AmbientRand(f32 lo, f32 hi) {
  ambient_rng_ ^= ambient_rng_ << 13;
  ambient_rng_ ^= ambient_rng_ >> 7;
  ambient_rng_ ^= ambient_rng_ << 17;
  return lo + (hi - lo) * static_cast<f32>((ambient_rng_ >> 40) & 0xffffff) / 16777216.0f;
}

void NpcDirector::UpdateAmbient(f32 dt) {
  // Host / single-player only; a client receives this motion via actor sync (like
  // followers), so it must not double-simulate (the RNG would diverge).
#if RECREATION_HAS_NET
  if (!actors_->HasPlayer() || ctx_.client_session) {
    ambient_.clear();
    return;
  }
#else
  if (!actors_->HasPlayer()) {
    ambient_.clear();
    return;
  }
#endif
  Vec3 ppos;
  if (!actors_->PlayerWorldPos(&ppos)) return;

  constexpr f32 kActiveRadius = 26.0f;  // only NPCs near the player sandbox
  constexpr f32 kFaceRadius = 6.0f;     // turn to face the player within this
  constexpr f32 kWanderRadius = 4.5f;   // how far an idle goal sits from home
  constexpr f32 kIdleMin = 3.0f, kIdleMax = 7.0f;
  constexpr f32 kWalkSpeed = 1.3f;
  constexpr int kMaxStepsPerFrame = 8;  // cap the per-frame steering cost

  // Gather the near, unmanaged NPCs first (no ECS mutation during iteration).
  struct Nearby {
    ecs::Entity e;
    u64 handle;
    world::Transform* t;
    f32 dist2;
  };
  base::Vector<Nearby> nearby;
  world_.Each<world::Npc, world::FormLink, world::Transform>(
      [&](ecs::Entity e, world::Npc&, world::FormLink& link, world::Transform& t) {
        const u64 handle = link.form.packed();
        if (followers_.find(handle) || guides_.find(handle)) return;  // driven elsewhere
        if (combat_.find(handle) || world_.Has<world::Dead>(e)) return;  // fighting / downed
        if (const world::CombatTeam* ct = world_.Get<world::CombatTeam>(e))
          if (ct->team != 0) return;  // a soldier awaiting orders, not an idler
        const f32 dx = t.position[0] - ppos.x, dz = t.position[2] - ppos.z;
        const f32 d2 = dx * dx + dz * dz;
        if (d2 <= kActiveRadius * kActiveRadius) nearby.push_back({e, handle, &t, d2});
      });

  base::UnorderedMap<u64, AmbientState> kept;  // rebuilt, pruning NPCs no longer near
  int steps = 0;
  for (const Nearby& n : nearby) {
    AmbientState state;
    if (const AmbientState* prev = ambient_.find(n.handle)) {
      state = *prev;
    } else {
      state.home = {n.t->position[0], n.t->position[1], n.t->position[2]};
      state.idle_timer = AmbientRand(kIdleMin, kIdleMax);
    }
    if (state.walking) {
      if (steps < kMaxStepsPerFrame) {  // over the cap: stay walking, resume next frame
        ++steps;
        const float goal[3] = {state.goal.x, state.goal.y, state.goal.z};
        if (!StepNpcSteering(n.e, goal, n.t->position, n.t->rotation, kWalkSpeed, 0.6f, 0.4f, dt)) {
          state.walking = false;
          state.idle_timer = AmbientRand(kIdleMin, kIdleMax);
        }
      }
    } else {
      state.idle_timer -= dt;
      if (n.dist2 < kFaceRadius * kFaceRadius) {  // acknowledge a close player
        const f32 yaw = std::atan2(ppos.x - n.t->position[0], ppos.z - n.t->position[2]);
        actors_->SetNpcGait(n.e, 0.0f, true, yaw);
      }
      if (state.idle_timer <= 0.0f) {  // wander to a new spot near home
        const f32 ang = AmbientRand(0.0f, 6.2831853f);
        const f32 r = AmbientRand(1.0f, kWanderRadius);
        state.goal = {state.home.x + std::cos(ang) * r, n.t->position[1],
                      state.home.z + std::sin(ang) * r};
        state.walking = true;
      }
    }
    kept.insert(n.handle, state);
  }
  ambient_ = std::move(kept);
}

namespace {
constexpr u64 kPlayerHandle = 0x14;  // Skyrim's PlayerRef form id
}  // namespace

void NpcDirector::EnterCombat(u64 attacker, u64 target) {
  if (attacker == 0 || target == 0 || attacker == target) return;
  // The player fights through its own input, never the auto-attacker driver; it
  // can still be a target (NPCs close on and swing at it).
  if (attacker == kPlayerHandle) return;
  CombatState* cs = combat_.find(attacker);
  if (cs) {
    cs->target = target;
  } else {
    CombatState fresh;
    fresh.target = target;
    // Stagger swing phases so a spawned squad does not strike in lockstep.
    fresh.rng = static_cast<u32>(0x9e3779b9u ^ (attacker * 2654435761u));
    fresh.swing_timer = combat_params_.swing_seconds *
                        (static_cast<f32>(fresh.rng >> 24) / 256.0f);
    combat_.insert(attacker, fresh);
  }
}

void NpcDirector::LeaveCombat(u64 attacker) { combat_.erase(attacker); }

void NpcDirector::OnActorDied(u64 actor) {
  combat_.erase(actor);  // the dead actor stops fighting
  // Anyone fighting the dead actor disengages (their target is now a corpse).
  base::Vector<u64> idle;
  combat_.ForEach([&](u64 attacker, CombatState& cs) {
    if (cs.target == actor) idle.push_back(attacker);
  });
  for (u64 a : idle) combat_.erase(a);
  // Tag the body so auto-aggression skips it and it stops moving.
  ecs::Entity e;
  world::Transform* t;
  if (ResolveCombatant(actor, &e, &t) && !world_.Has<world::Dead>(e)) {
    world_.Add(e, world::Dead{});
    actors_->SetNpcGait(e, 0.0f, false, 0.0f);
  }
}

void NpcDirector::OnActorResurrected(u64 actor) {
  // Clear the downed tag so the actor renders upright and is eligible to fight
  // again, the mirror of OnActorDied, driven by a script Reset/Resurrect.
  ecs::Entity e;
  world::Transform* t;
  if (ResolveCombatant(actor, &e, &t) && world_.Has<world::Dead>(e))
    world_.Remove<world::Dead>(e);
}

void NpcDirector::AcquireTargets() {
  // Teamed, living actors that are not already fighting pick the nearest hostile
  // (different non-zero team) within engage range. This drives the opening clash
  // and re-acquisition after a kill, so a battle sustains itself. Bounded scan;
  // teams are assigned by the spawner (it knows each soldier's side).
  struct Cand {
    u64 handle;
    i32 team;
    float pos[3];
  };
  base::Vector<Cand> cands;
  world_.Each<world::Npc, world::CombatTeam, world::FormLink, world::Transform>(
      [&](ecs::Entity e, world::Npc&, world::CombatTeam& ct, world::FormLink& link,
          world::Transform& t) {
        if (ct.team == 0 || world_.Has<world::Dead>(e)) return;
        cands.push_back({link.form.packed(), ct.team, {t.position[0], t.position[1], t.position[2]}});
      });
  // The player can fight on a side (so NPCs target it); it is a target only, never
  // an auto-attacker (EnterCombat ignores the player handle).
  Vec3 ppos;
  if (player_team_ != 0 && actors_->HasPlayer() && actors_->PlayerWorldPos(&ppos))
    cands.push_back({kPlayerHandle, player_team_, {ppos.x, ppos.y, ppos.z}});

  const f32 r2 = combat_params_.engage_radius * combat_params_.engage_radius;
  for (const Cand& self : cands) {
    if (self.handle == kPlayerHandle) continue;        // player picks targets by input
    if (combat_.find(self.handle)) continue;           // already engaged
    u64 best = 0;
    f32 best_d2 = r2;
    for (const Cand& other : cands) {
      if (other.team == self.team || other.handle == self.handle) continue;
      const f32 d2 = world::PlanarDist2(self.pos, other.pos);
      if (d2 < best_d2) {
        best_d2 = d2;
        best = other.handle;
      }
    }
    if (best) EnterCombat(self.handle, best);
  }
}

bool NpcDirector::ResolveCombatant(u64 handle, ecs::Entity* entity, world::Transform** transform) {
  ecs::Entity e;
  if (handle == kPlayerHandle) {
    if (!actors_->HasPlayer()) return false;
    e = actors_->PlayerEntity();
  } else {
    e = ctx_.quest_world ? ctx_.quest_world->Find(handle) : ecs::Entity{};
  }
  if (!world_.IsAlive(e)) return false;
  world::Transform* t = world_.Get<world::Transform>(e);
  if (!t) return false;
  *entity = e;
  *transform = t;
  return true;
}

void NpcDirector::UpdateCombat(f32 dt) {
  // Host / single-player authoritative; a client receives soldier motion via
  // actor sync and deaths via quest/actor replication, so it must not simulate.
#if RECREATION_HAS_NET
  if (ctx_.client_session) return;
#endif
  // Re-acquire targets a few times a second (cheaper than every frame and the
  // clash cadence does not need frame precision).
  combat_acquire_timer_ -= dt;
  if (combat_acquire_timer_ <= 0.0f) {
    combat_acquire_timer_ = 0.3f;
    AcquireTargets();
  }
  if (combat_.empty()) return;
  const world::CombatParams& p = combat_params_;

  struct Hit {
    u64 attacker;
    u64 target;
    f32 damage;
  };
  std::vector<Hit> hits;
  base::Vector<u64> drop;  // attackers whose target vanished or fled

  combat_.ForEach([&](u64 attacker, CombatState& cs) {
    ecs::Entity ae;
    world::Transform* at;
    if (!ResolveCombatant(attacker, &ae, &at)) return;  // not streamed yet: keep enrolled
    ecs::Entity te;
    world::Transform* tt;
    if (cs.target == 0 || !ResolveCombatant(cs.target, &te, &tt)) {
      drop.push_back(attacker);
      return;
    }
    if (world::PlanarDist2(at->position, tt->position) >
        p.give_up_radius * p.give_up_radius) {
      drop.push_back(attacker);
      return;
    }
    cs.swing_timer -= dt;
    if (world::WithinPlanar(at->position, tt->position, p.melee_reach)) {
      // In reach: face the target, hold position, and swing on the cooldown.
      const f32 yaw = std::atan2(tt->position[0] - at->position[0],
                                 tt->position[2] - at->position[2]);
      const f32 h = yaw * 0.5f;
      at->rotation[0] = 0;
      at->rotation[1] = std::sin(h);
      at->rotation[2] = 0;
      at->rotation[3] = std::cos(h);
      actors_->SetNpcGait(ae, 0.0f, true, yaw);
      if (cs.swing_timer <= 0.0f) {
        cs.swing_timer = p.swing_seconds;
        cs.rng = cs.rng * 1664525u + 1013904223u;
        const f32 roll = static_cast<f32>((cs.rng >> 8) & 0xffffff) / 16777216.0f;
        hits.push_back({attacker, cs.target, world::SwingDamage(p, roll)});
      }
    } else {
      // Out of reach: run at the target.
      const float goal[3] = {tt->position[0], tt->position[1], tt->position[2]};
      StepNpcSteering(ae, goal, at->position, at->rotation, p.approach_speed,
                      p.melee_reach * 0.9f, p.melee_reach * 0.8f, dt);
    }
  });

  for (u64 a : drop) combat_.erase(a);

  // Apply the swings on the guest thread, where actor values and the OnDeath ->
  // quest/managed path live. Throttled by the per-attacker swing cooldown, so
  // this stays a small batch even in a large battle.
  if (!hits.empty() && ctx_.scripts && ctx_.bindings) {
    auto* binds = ctx_.bindings;
    ctx_.scripts->guest().Submit(
        [binds, hits = std::move(hits)](rec::script::papyrus::VirtualMachine&) {
          for (const Hit& h : hits)
            binds->ApplyMeleeHit(script::papyrus::ObjectRef{h.attacker},
                                 script::papyrus::ObjectRef{h.target}, h.damage);
        });
  }
}

bool NpcDirector::PlayerMeleeStrike(const Vec3& pos, f32 yaw) {
  if (!ctx_.scripts || !ctx_.bindings) return false;
  // Swing in the facing direction: the nearest living NPC inside the reach and a
  // forward arc takes the hit. The player aims, so any actor in front is fair
  // game (it is how you cut down the fort's bandits).
  const float self[3] = {pos.x, pos.y, pos.z};
  const float fwd[3] = {std::sin(yaw), 0.0f, -std::cos(yaw)};
  constexpr f32 kReach = 3.0f;    // a touch longer than the NPC reach
  constexpr f32 kArcCos = 0.35f;  // ~70 degrees to each side
  u64 best = 0;
  f32 best_d2 = kReach * kReach;
  world_.Each<world::Npc, world::FormLink, world::Transform>(
      [&](ecs::Entity e, world::Npc&, world::FormLink& link, world::Transform& t) {
        if (world_.Has<world::Dead>(e)) return;
        if (!world::InMeleeArc(self, t.position, fwd, kReach, kArcCos)) return;
        const f32 dx = t.position[0] - self[0], dz = t.position[2] - self[2];
        const f32 d2 = dx * dx + dz * dz;
        if (d2 >= best_d2) return;
        best_d2 = d2;
        best = link.form.packed();
      });
  if (!best) return false;
  auto* binds = ctx_.bindings;
  ctx_.scripts->guest().Submit([binds, best](rec::script::papyrus::VirtualMachine&) {
    // A heavier blow than a common soldier's: the player is the dragonborn.
    binds->ApplyMeleeHit(script::papyrus::ObjectRef{kPlayerHandle},
                         script::papyrus::ObjectRef{best}, 42.0f);
  });
  REC_DEBUG("player struck 0x{:x}", best);
  return true;
}

bool NpcDirector::BattleStrength(int* team_a, int* team_b, int* fallen) const {
  if (!cw_battle_active_ && !cw_field_active_) return false;
  int a = 0, b = 0, d = 0;
  world_.Each<world::CombatTeam>([&](ecs::Entity e, world::CombatTeam& ct) {
    if (world_.Has<world::Dead>(e)) {
      ++d;
      return;
    }
    if (ct.team == 1)
      ++a;
    else if (ct.team == 2)
      ++b;
  });
  *team_a = a;
  *team_b = b;
  *fallen = d;
  return true;
}

bool NpcDirector::BattleGauges(f32* team1_frac, f32* team2_frac) const {
  // When the clash is tied to the siege's reinforcement pool, the bars read the
  // live CWReinforcementPool globals, the OG quest mechanic, depleting as the
  // soldiers on screen fall, instead of a plain head-count.
  if (cw_siege_pool_seeded_) {
    *team1_frac =
        cw_pool_atk_start_ > 0 ? std::clamp(cw_pool_atk_cur_ / cw_pool_atk_start_, 0.0f, 1.0f) : 0.0f;
    *team2_frac =
        cw_pool_def_start_ > 0 ? std::clamp(cw_pool_def_cur_ / cw_pool_def_start_, 0.0f, 1.0f) : 0.0f;
    return true;
  }
  int a, b, d;
  if (!BattleStrength(&a, &b, &d)) return false;
  *team1_frac = cw_start1_ > 0 ? static_cast<f32>(a) / static_cast<f32>(cw_start1_) : 0.0f;
  *team2_frac = cw_start2_ > 0 ? static_cast<f32>(b) / static_cast<f32>(cw_start2_) : 0.0f;
  return true;
}

void NpcDirector::CwBattleTick(f32 dt) {
  if (!cw_battle_pending_ || !actors_->HasPlayer() || !ctx_.scripts || !ctx_.bindings) return;

  if (!cw_battle_active_) {
    Vec3 ppos;
    if (!actors_->PlayerWorldPos(&ppos)) return;
    // Enlist the streamed NPCs around the player into two armies, split across the
    // player's position so they form opposing lines and charge.
    base::Vector<u64> enlisted;
    Vec3 foe_sum{};
    int foes = 0;
    world_.Each<world::Npc, world::FormLink, world::Transform>(
        [&](ecs::Entity e, world::Npc&, world::FormLink& link, world::Transform& t) {
          const f32 dx = t.position[0] - ppos.x, dz = t.position[2] - ppos.z;
          if (dx * dx + dz * dz > 200.0f * 200.0f) return;  // grab the whole garrison
          if (world_.Has<world::Dead>(e)) return;
          const i32 team = t.position[0] < ppos.x ? 1 : 2;
          world_.Add(e, world::CombatTeam{team});
          enlisted.push_back(link.form.packed());
          if (team == 2) {  // the player fights team 1, so team 2 is the enemy line
            foe_sum = foe_sum + Vec3{t.position[0], t.position[1], t.position[2]};
            ++foes;
          }
        });
    if (enlisted.size() < 2) return;  // wait for the cell's NPCs to stream in
    // Turn the player (hence the follow camera) to face the enemy line, so the
    // melee plays out in view and a forward swing connects.
    if (foes > 0) {
      const Vec3 c = foe_sum * (1.0f / static_cast<f32>(foes));
      ctx_.cam_yaw = std::atan2(c.x - ppos.x, -(c.z - ppos.z));
    }
    // Give every soldier a battle health pool on the guest thread, so the melee
    // resolves in seconds rather than the default 100 hp grind.
    std::vector<u64> handles(enlisted.begin(), enlisted.end());
    auto* binds = ctx_.bindings;
    ctx_.scripts->guest().Submit([binds, handles](rec::script::papyrus::VirtualMachine&) {
      for (u64 h : handles) binds->SetActorValue(script::papyrus::ObjectRef{h}, "health", 80.0f);
    });
    cw_battle_active_ = true;
    cw_start2_ = foes;
    cw_start1_ = static_cast<int>(enlisted.size()) - foes;  // for the reinforcement bars
    player_team_ = 1;  // the player fights on team 1; team-2 soldiers will target it
    REC_INFO("cw battle: enlisted {} soldiers into two armies", enlisted.size());
  }

  cw_battle_log_timer_ -= dt;
  if (cw_battle_log_timer_ <= 0.0f) {
    cw_battle_log_timer_ = 1.0f;
    int a = 0, b = 0, d = 0;
    BattleStrength(&a, &b, &d);
    REC_INFO("cw battle: team1={} team2={} fallen={} engaged={}", a, b, d, combatant_count());
  }
}

bool NpcDirector::BattleCam(Vec3* eye, Vec3* target) const {
  if (!cw_field_active_) return false;
  // A near ground-level vantage behind team 1 looking ACROSS the clash, like the
  // original game's field battles: the soldiers read against the horizon (terrain
  // + sky) instead of a steep top-down look into terrain that the camera is too
  // far above for the streamer to have drawn. Aim at chest height of the centre.
  *target = cw_field_center_ + Vec3{0.0f, 1.1f, 0.0f};
  *eye = cw_field_center_ - cw_field_fwd_ * 12.0f + Vec3{0.0f, 3.0f, 0.0f};
  return true;
}

void NpcDirector::Cw00DemoTick(f32 dt) {
  if (!cw00_demo_pending_ || !actors_->HasPlayer() || !interaction_) return;
  // Castle Dour war room actors (Skyrim.esm, plugin 0): the placed ACHR refs of
  // General Tullius and Legate Rikke, and Tullius's CW00A forcegreet INFO whose
  // fragment is GetOwningQuest().SetStage(10), the enlistment.
  constexpr u64 kTullius = 0x000198BA;
  constexpr u64 kRikke = 0x000198BB;
  constexpr u64 kTulliusForcegreetInfo = 0x000C348B;
  const u64 target = cw00_demo_phase_ == 0 ? kTullius : kRikke;
  cw00_demo_timer_ += dt;

  Vec3 ppos;
  if (!actors_->PlayerWorldPos(&ppos)) return;

  // Steer toward the officer if its ref has streamed in (cosmetic); a forcegreet
  // has range, so fire within 8 m or after a grace period, a wedged walk (the
  // war room is multi-level and the flat router cannot always climb to Tullius)
  // still lands the beat.
  bool arrived = false;
  ecs::Entity e;
  world::Transform* t;
  if (ResolveCombatant(target, &e, &t)) {
    ctx_.auto_walk = true;
    ctx_.auto_walk_goal = {t->position[0], t->position[1], t->position[2]};
    ctx_.auto_walk_has_goal = true;
    const float self[3] = {ppos.x, ppos.y, ppos.z};
    arrived = world::WithinPlanar(self, t->position, 8.0f);
  }
  if (!arrived && cw00_demo_timer_ < 14.0f) return;  // still closing the distance

  if (cw00_demo_phase_ == 0) {
    // Reached Tullius: he greets the player and the enlistment is acknowledged.
    constexpr u64 kCw00aQuest = 0x000D3C5F;  // CW00A, the INFO's owning quest
    interaction_->RunInfoFragment(kTulliusForcegreetInfo, kCw00aQuest);
    if (ctx_.scripts && ctx_.bindings) {
      auto* binds = ctx_.bindings;
      constexpr u64 kCw00a = 0x000D3C5F;  // CW00A "Imperial Introductions"
      ctx_.scripts->guest().Submit([binds](rec::script::papyrus::VirtualMachine&) {
        REC_INFO("cw00 demo: CW00A is now at stage {} after the forcegreet",
                 binds->GetStage(script::papyrus::ObjectRef{kCw00a}));
      });
    }
    REC_INFO("cw00 demo: General Tullius hails the player -- enlisted (CW00A stage 10)");
    cw00_demo_phase_ = 1;
    cw00_demo_timer_ = 0;
  } else {
    // Reached Legate Rikke: the player reports in for the fort assignment.
    ctx_.auto_walk = false;
    ctx_.auto_walk_has_goal = false;
    cw00_demo_pending_ = false;
    REC_INFO("cw00 demo: reported to Legate Rikke for the fort assignment");
  }
}

u64 NpcDirector::SpawnSoldier(const Vec3& pos, i32 team) {
  // Synthetic handle in the reserved 0xFFFF plugin slot, offset high so it never
  // collides with quest PlaceAtMe spawns (which start low in the same slot).
  const u64 id = 0x40000000u + cw_spawn_seq_++;
  const u64 handle = (0xFFFFull << 32) | id;
  ecs::Entity e = world_.Create();
  world::Transform t;
  t.position[0] = pos.x;
  t.position[1] = GroundY(pos.x, pos.z, pos.y);
  t.position[2] = pos.z;
  world_.Add(e, t);
  world_.Add(e, world::Npc{bethesda::GlobalFormId{}});  // generic soldier; rig is shared
  world_.Add(e, world::FormLink{bethesda::GlobalFormId{0xFFFF, static_cast<u32>(id)}});
  world_.Add(e, world::CombatTeam{team});
  if (ctx_.quest_world) ctx_.quest_world->Register(handle, e);
  // Replicate the spawn so a client builds the same biped, registered under the
  // same form handle; the actor sync then streams the host's movement to it. The
  // client does not simulate combat, but it carries the team so it renders the
  // matching faction armour/tint instead of the bare civilian body.
  world::WorldCommand c;
  c.op = world::WorldOp::kSpawn;
  c.handle = handle;
  c.is_actor = true;
  c.team = team;
  c.pos = {t.position[0], t.position[1], t.position[2]};
  replicated_spawns_.push_back(c);
  return handle;
}

void NpcDirector::SeedSiegeReinforcementPools() {
  if (cw_siege_quest_ == 0 || cw_siege_master_ == 0 || !ctx_.scripts || !ctx_.bindings) return;
  // The pool starts at this clash's strength: one point per soldier on each side.
  // Team 1 (the player's line) assaults, team 2 holds.
  int atk = 0, def = 0;
  for (u64 h : cw_field_soldiers_) {
    ecs::Entity e = ctx_.quest_world ? ctx_.quest_world->Find(h) : ecs::kInvalidEntity;
    const world::CombatTeam* ct = world_.Get<world::CombatTeam>(e);
    if (ct && ct->team == 1)
      ++atk;
    else
      ++def;
  }
  const u64 siege = cw_siege_quest_, master = cw_siege_master_;
  auto* binds = ctx_.bindings;
  // Seed on the script thread (the only place the siege instance is safe to
  // touch) and hand back the two pool globals the controller writes through.
  const std::pair<u64, u64> globals =
      ctx_.scripts->guest()
          .SubmitFor([siege, master, atk, def, binds](rec::script::papyrus::VirtualMachine& vm) {
            using rec::script::papyrus::ObjectRef;
            using rec::script::papyrus::Value;
            using rec::script::papyrus::ValueType;
            auto set_member = [&](const char* name, Value v) {
              if (Value* m = vm.MemberVar(ObjectRef{siege}, name)) *m = std::move(v);
            };
            // Wire the controller to the CW master (whose properties carry the
            // reinforcement-pool globals) and make the pools finite so a death
            // actually spends one (an infinite pool no-ops by design).
            set_member("::CWs_var", Value::Object(ObjectRef{master}));
            set_member("::InfiniteRespawnAttacker_var", Value::Bool(false));
            set_member("::InfiniteRespawnDefender_var", Value::Bool(false));
            set_member("::PoolAttacker_var", Value::Float(static_cast<f32>(atk)));
            set_member("::StartingPoolAttacker_var", Value::Float(static_cast<f32>(atk)));
            set_member("::PoolDefender_var", Value::Float(static_cast<f32>(def)));
            set_member("::StartingPoolDefender_var", Value::Float(static_cast<f32>(def)));
            auto global_handle = [&](const char* prop) -> u64 {
              Value g = vm.GetProperty(ObjectRef{master}, prop);
              return g.type() == ValueType::kObject ? g.as_object().handle : 0;
            };
            const u64 atk_g = global_handle("CWReinforcementPoolAttacker");
            const u64 def_g = global_handle("CWReinforcementPoolDefender");
            if (atk_g) binds->SetGlobalValue(ObjectRef{atk_g}, static_cast<f32>(atk));
            if (def_g) binds->SetGlobalValue(ObjectRef{def_g}, static_cast<f32>(def));
            return std::pair<u64, u64>{atk_g, def_g};
          })
          .get();
  cw_pool_atk_start_ = cw_pool_atk_cur_ = static_cast<f32>(atk);
  cw_pool_def_start_ = cw_pool_def_cur_ = static_cast<f32>(def);
  cw_siege_pool_seeded_ = true;
  REC_INFO("cw siege: reinforcement pool seeded (attackers {}, defenders {}); globals 0x{:x}/0x{:x}",
           atk, def, globals.first, globals.second);
}

void NpcDirector::ChargeSiegeReinforcementDeaths() {
  if (!cw_siege_pool_seeded_ || !ctx_.scripts) return;
  // Each soldier that newly fell spends one point of its side's pool through the
  // siege's own ModifyPool, so the on-screen losses drive the authored mechanic.
  std::vector<bool> attacker_died;
  for (u64 h : cw_field_soldiers_) {
    if (cw_pool_counted_.count(h)) continue;
    ecs::Entity e = ctx_.quest_world ? ctx_.quest_world->Find(h) : ecs::kInvalidEntity;
    if (!world_.IsAlive(e) || !world_.Has<world::Dead>(e)) continue;
    const world::CombatTeam* ct = world_.Get<world::CombatTeam>(e);
    cw_pool_counted_.insert(h);
    const bool attacker = ct && ct->team == 1;
    attacker_died.push_back(attacker);
    // Mirror the spend on the main thread so the HUD bars never read the live
    // global the script thread is writing.
    if (attacker)
      cw_pool_atk_cur_ = std::max(0.0f, cw_pool_atk_cur_ - 1.0f);
    else
      cw_pool_def_cur_ = std::max(0.0f, cw_pool_def_cur_ - 1.0f);
  }
  if (attacker_died.empty()) return;
  const u64 siege = cw_siege_quest_;
  ctx_.scripts->guest().Submit(
      [siege, attacker_died](rec::script::papyrus::VirtualMachine& vm) {
        using rec::script::papyrus::ObjectRef;
        using rec::script::papyrus::Value;
        for (bool attacker : attacker_died)
          vm.Call(ObjectRef{siege}, "ModifyPool", {Value::Bool(attacker), Value::Int(-1)});
      });
}

void NpcDirector::CwFieldBattleTick(f32 dt) {
  if (!cw_field_pending_ || !actors_->HasPlayer()) return;
  if (!cw_field_active_) {
    cw_field_warmup_ += dt;
    // Let the terrain stream and the player settle (and, in MP, a client finish
    // joining so it receives the live spawn broadcast). REC_CW_BATTLE_DELAY tunes it.
    f32 warmup = 2.0f;
    if (CwBattleDelay.overridden()) warmup = CwBattleDelay.get();
    if (cw_field_warmup_ < warmup) return;
    Vec3 ppos;
    if (!actors_->PlayerWorldPos(&ppos)) return;
    // Pick the direction that stages the clash on the FLATTEST nearby ground:
    // scoring purely by "most open" sends the battle off a clifftop (a drop is
    // maximally clear), which is exactly the washed-out mid-air framing we want
    // to avoid. Sample the terrain heightmap a fixed reach out in each direction
    // and prefer the smallest drop from the player's own ground, lightly favouring
    // open ground (no wall) so the clash is not staged into a rock face.
    constexpr f32 kReach = 14.0f;
    const f32 player_ground = GroundY(ppos.x, ppos.z, ppos.y);
    f32 yaw = ctx_.cam_yaw;
    f32 best_score = -1e9f;
    for (int i = 0; i < 12; ++i) {
      const f32 a = ctx_.cam_yaw + static_cast<f32>(i) * 30.0f * 0.0174533f;
      const Vec3 d{std::sin(a), 0.0f, -std::cos(a)};
      const Vec3 c = ppos + d * kReach;
      const f32 cg = GroundY(c.x, c.z, player_ground);
      const f32 drop = std::abs(cg - player_ground);
      f32 open = kReach;
      if (physics_.initialized()) {
        physics::PhysicsWorld::RayHit hit;
        if (physics_.Raycast(Vec3{ppos.x, ppos.y + 1.5f, ppos.z}, d, kReach, &hit))
          open = hit.distance;
      }
      // A lake reads as perfectly flat, so pure flatness would stage the clash out
      // over the water (soldiers on the lakebed, camera facing the sheet). Reject
      // any direction whose ground sits under the local water surface.
      f32 wh = 0;
      Vec3 flow;
      const bool wet = ctx_.streamer &&
                       ctx_.streamer->WaterHeightAt(Vec3{c.x, cg, c.z}, &wh, &flow) && wh > cg + 0.5f;
      // Flatness dominates (metres of drop), with a small open-ground bonus.
      const f32 score = -drop + open * 0.1f - (wet ? 1000.0f : 0.0f);
      if (score > best_score) {
        best_score = score;
        yaw = a;
      }
    }
    const f32 clearance = kReach;
    ctx_.cam_yaw = yaw;
    const Vec3 fwd{std::sin(yaw), 0.0f, -std::cos(yaw)};
    const Vec3 right{std::cos(yaw), 0.0f, std::sin(yaw)};
    const f32 reach = std::clamp(clearance * 0.6f, 11.0f, 20.0f);
    Vec3 center = ppos + fwd * reach;  // stage the clash on the open ground ahead
    // Drop the clash anchor onto the ground: ppos sits at eye height, so leaving
    // center there makes the battle camera look at a point in mid-air and frames
    // the (ground-level) soldiers against the sky. GroundY puts the look target,
    // and the spawn ring, on the terrain the soldiers actually stand on.
    center.y = GroundY(center.x, center.z, ppos.y);
    cw_field_center_ = center;
    cw_field_fwd_ = fwd;
    constexpr int kPerSide = 8;
    constexpr f32 kSpacing = 2.4f;
    constexpr f32 kHalfGap = 5.0f;  // the two lines start 10 m apart and charge
    for (int i = 0; i < kPerSide; ++i) {
      const f32 off = (static_cast<f32>(i) - (kPerSide - 1) * 0.5f) * kSpacing;
      cw_field_soldiers_.push_back(SpawnSoldier(center - fwd * kHalfGap + right * off, 1));
      cw_field_soldiers_.push_back(SpawnSoldier(center + fwd * kHalfGap + right * off, 2));
    }
    std::vector<u64> handles(cw_field_soldiers_.begin(), cw_field_soldiers_.end());
    auto* binds = ctx_.bindings;
    if (binds && ctx_.scripts)
      ctx_.scripts->guest().Submit([binds, handles](rec::script::papyrus::VirtualMachine&) {
        // The soldiers are pushed team1, team2, team1, ... so even indices are the
        // player's assaulting line. Give it the edge so the siege resolves as a
        // win (the fort is taken) rather than dragging to the grace timeout; the
        // garrison still fights hard and both sides spend their reinforcements.
        for (size_t i = 0; i < handles.size(); ++i)
          binds->SetActorValue(script::papyrus::ObjectRef{handles[i]}, "health",
                               (i % 2 == 0) ? 220.0f : 120.0f);
      });
    cw_field_active_ = true;
    cw_start1_ = cw_start2_ = kPerSide;  // for the reinforcement bars
    player_team_ = 1;  // the player fights on the near line; team 2 targets it too
    REC_INFO("cw field battle: spawned {} soldiers in two lines", cw_field_soldiers_.size());
    SeedSiegeReinforcementPools();
    // Deferred REC_CW_SIDE enlistment: advance the intro quest now (the managed
    // allegiance tracker is up by the time the battle stages), so the won siege
    // captures the fort for the player's chosen side.
    if (cw_enlist_quest_ != 0 && ctx_.scripts && ctx_.bindings) {
      const u64 join = cw_enlist_quest_;
      cw_enlist_quest_ = 0;
      auto* binds = ctx_.bindings;
      ctx_.scripts->guest().Submit([binds, join](rec::script::papyrus::VirtualMachine&) {
        binds->StartQuest(script::papyrus::ObjectRef{join});
        binds->SetStage(script::papyrus::ObjectRef{join}, 10);
      });
    }
  }

  int a = 0, b = 0, d = 0;
  world_.Each<world::CombatTeam>([&](ecs::Entity e, world::CombatTeam& ct) {
    if (world_.Has<world::Dead>(e)) {
      ++d;
      return;
    }
    if (ct.team == 1)
      ++a;
    else if (ct.team == 2)
      ++b;
  });

  cw_battle_log_timer_ -= dt;
  if (cw_battle_log_timer_ <= 0.0f) {
    cw_battle_log_timer_ = 1.0f;
    if (cw_siege_pool_seeded_) {
      REC_INFO("cw field battle: team1={} team2={} fallen={} | reinforcement pool atk={} def={}", a,
               b, d, cw_pool_atk_cur_, cw_pool_def_cur_);
    } else {
      REC_INFO("cw field battle: team1={} team2={} fallen={} engaged={}", a, b, d,
               combatant_count());
    }
  }

  ChargeSiegeReinforcementDeaths();

  // Periodically re-queue the living soldiers' spawns so a client that joined
  // after the opening broadcast still builds them (the client apply is idempotent
  // by handle, so already-synced clients ignore the repeat).
  cw_field_resync_ -= dt;
  if (cw_field_resync_ <= 0.0f) {
    cw_field_resync_ = 3.0f;
    for (u64 h : cw_field_soldiers_) {
      ecs::Entity e = ctx_.quest_world ? ctx_.quest_world->Find(h) : ecs::kInvalidEntity;
      if (!world_.IsAlive(e)) continue;
      const world::Transform* t = world_.Get<world::Transform>(e);
      if (!t) continue;
      world::WorldCommand c;
      c.op = world::WorldOp::kSpawn;
      c.handle = h;
      c.is_actor = true;
      if (const world::CombatTeam* ct = world_.Get<world::CombatTeam>(e)) c.team = ct->team;
      c.pos = {t->position[0], t->position[1], t->position[2]};
      replicated_spawns_.push_back(c);
    }
  }

  // Resolve the battle into the quest: the enemy line is broken, or a grace
  // timeout passed (a wedged fight should not stall the quest). Advances the
  // configured quest stage via its real fragment.
  cw_battle_grace_ += dt;
  if (!cw_battle_resolved_ && cw_battle_quest_ != 0 && cw_battle_win_stage_ >= 0 &&
      (b == 0 || cw_battle_grace_ > 45.0f)) {
    cw_battle_resolved_ = true;
    const u64 quest = cw_battle_quest_;
    const i32 stage = cw_battle_win_stage_;
    auto* binds = ctx_.bindings;
    if (binds && ctx_.scripts)
      ctx_.scripts->guest().Submit([binds, quest, stage](rec::script::papyrus::VirtualMachine&) {
        binds->SetStage(script::papyrus::ObjectRef{quest}, stage);
      });
    // Modern battle summary: close the clash with an outcome banner and the toll
    // on each side, the way a strategy game does (the OG quest just flips a stage).
    if (ctx_.game_ui) {
      const int your_fell = std::max(0, cw_start1_ - a);
      const int enemy_fell = std::max(0, cw_start2_ - b);
      ctx_.game_ui->FlashQuestUpdate("Victory! The fort is taken  (your losses " +
                                     std::to_string(your_fell) + ", enemy losses " +
                                     std::to_string(enemy_fell) + ")");
    }
    REC_INFO("cw field battle: enemy line broken ({} left) -> quest 0x{:x} stage {}", b, quest,
             stage);
  }
}

void NpcDirector::UpdateFollowers(f32 dt) {
  // Host authoritative: a client receives follower motion via actor sync.
#if RECREATION_HAS_NET
  if (followers_.empty() || !actors_->HasPlayer() || ctx_.client_session) return;
#else
  if (followers_.empty() || !actors_->HasPlayer()) return;
#endif
  Vec3 ppos;
  if (!actors_->PlayerWorldPos(&ppos)) return;
  const float leader_pos[3] = {ppos.x, ppos.y, ppos.z};
  const float leader_yaw = actors_->PlayerYaw();

  // Collect the follower transforms, with positions kept flat for separation.
  struct Follower {
    ecs::Entity entity;
    i32 slot;
    world::Transform* transform;
  };
  base::Vector<Follower> followers;
  base::Vector<float> positions;  // xyz per follower, parallel to `followers`
  world_.Each<world::Npc, world::FormLink, world::Transform>(
      [&](ecs::Entity e, world::Npc&, world::FormLink& link, world::Transform& t) {
        const i32* slot = followers_.find(link.form.packed());
        if (!slot) return;
        followers.push_back({e, *slot, &t});
        positions.push_back(t.position[0]);
        positions.push_back(t.position[1]);
        positions.push_back(t.position[2]);
      });
  if (followers.empty()) return;

  const world::SteerParams params{2.6f, 2.2f, 1.5f};
  base::Vector<float> others;
  for (size_t i = 0; i < followers.size(); ++i) {
    float goal[3];
    world::FollowSlot(leader_pos, leader_yaw, followers[i].slot, 1.8f, goal);

    // Spread followers apart so they do not pile onto the same slot.
    others.clear();
    for (size_t j = 0; j < followers.size(); ++j) {
      if (j == i) continue;
      others.push_back(positions[j * 3 + 0]);
      others.push_back(positions[j * 3 + 1]);
      others.push_back(positions[j * 3 + 2]);
    }
    float sep[3];
    world::SeparationOffset(&positions[i * 3], others.data(),
                           static_cast<int>(others.size() / 3), 1.2f, sep);
    goal[0] += sep[0] * 0.6f;
    goal[2] += sep[2] * 0.6f;

    world::Transform* t = followers[i].transform;
    // A follower this far from the leader was stranded by a cell/load-door
    // transition the player walked through (the door teleports only the player).
    // Snap it to its slot behind the player so the escort regroups instead of
    // leaving companions in the previous cell.
    const f32 dx = t->position[0] - leader_pos[0], dz = t->position[2] - leader_pos[2];
    if (dx * dx + dz * dz > 20.0f * 20.0f) {
      t->position[0] = goal[0];
      t->position[1] = GroundY(goal[0], goal[2], leader_pos[1]);
      t->position[2] = goal[2];
      continue;
    }

    // Route the slot through pathfinding so a follower behind a wall rounds it
    // instead of pressing into it; close, clear slots resolve to a straight line.
    const Vec3 self{t->position[0], t->position[1], t->position[2]};
    const Vec3 wp = NavigateTo(self, Vec3{goal[0], goal[1], goal[2]});
    const float nav_goal[3] = {wp.x, wp.y, wp.z};
    StepNpcSteering(followers[i].entity, nav_goal, t->position, t->rotation, params.speed,
                    params.arrive_radius, params.stop_radius, dt);
  }
}

Vec3 NpcDirector::NavigateTo(const Vec3& from, const Vec3& goal) {
  if (!physics_.initialized()) return goal;
  // Route over a cached, cell-spanning navgrid (built from downward floor rays)
  // so an NPC or the player rounds interior walls toward the goal. The grid is
  // rebuilt only when the query point leaves the covered region, so most calls
  // are a cheap A* with no raycasting, unlike the old per-call grid.
  if (navgrid_.Empty() || !navgrid_.Covers(from)) {
    auto probe = [this, &from](f32 x, f32 z, f32* floor_y) {
      physics::PhysicsWorld::RayHit hit;
      if (physics_.Raycast(Vec3{x, from.y + 1.5f, z}, Vec3{0, -1, 0}, 4.0f, &hit)) {
        *floor_y = hit.position.y;
        return true;
      }
      return false;
    };
    navgrid_.Build(from, 11.0f, 0.8f, from.y, 0.6f, probe);
  }
  return navgrid_.Next(from, goal);
}

Vec3 NpcDirector::PathToward(const Vec3& from, const Vec3& goal) { return NavigateTo(from, goal); }

void NpcDirector::UpdateGuides(f32 dt) {
  // Host authoritative: a client receives guide motion via actor sync.
#if RECREATION_HAS_NET
  if (guides_.empty() || ctx_.client_session) return;
#else
  if (guides_.empty()) return;
#endif
  world_.Each<world::Npc, world::FormLink, world::Transform>(
      [&](ecs::Entity e, world::Npc&, world::FormLink& link, world::Transform& t) {
        const Vec3* target = guides_.find(link.form.packed());
        if (!target) return;
        const Vec3 wp = NavigateTo(Vec3{t.position[0], t.position[1], t.position[2]}, *target);
        const float goal[3] = {wp.x, wp.y, wp.z};
        StepNpcSteering(e, goal, t.position, t.rotation, 2.8f, 2.0f, 1.0f, dt);
      });
}

void NpcDirector::Mq101DemoTick(f32 dt) {
  if (!mq101_demo_pending_ || !actors_->HasPlayer() || !ctx_.scripts || !ctx_.bindings) return;

  // A live (unfired) demo waypoint means the player is still walking to it.
  // Reaching it fires the trigger normally; if the player gets stuck on terrain
  // or the quest's own fragment teleported them away, advance on a grace timeout
  // so the guided demo still completes.
  for (QuestMarker& m : quest_->markers())
    if (m.quest == mq101_demo_quest_ && !m.fired) {
      ctx_.auto_walk_goal = m.pos;  // steer the guided player to this waypoint
      ctx_.auto_walk_has_goal = true;
      mq101_demo_wait_ += dt;
      if (mq101_demo_wait_ > 12.0f) {
        m.fired = true;
        const u64 quest = mq101_demo_quest_;
        const i32 stage = m.advance_stage;
        auto* binds = ctx_.bindings;
        ctx_.scripts->guest().Submit([binds, quest, stage](script::papyrus::VirtualMachine&) {
          binds->SetStage(script::papyrus::ObjectRef{quest}, stage);
        });
        REC_INFO("demo: MQ101 waypoint timed out, advancing to stage {}", stage);
        mq101_demo_wait_ = 0.0f;
      }
      return;
    }

  mq101_demo_wait_ = 0.0f;
  if (mq101_demo_next_ >= mq101_demo_stages_.size()) {
    mq101_demo_pending_ = false;  // the last waypoint advanced the quest to completion
    ctx_.auto_walk_has_goal = false;
    REC_INFO("demo: MQ101 breadcrumb finished, quest driven to its completion stage");
    return;
  }

  const i32 advance_to = mq101_demo_stages_[mq101_demo_next_];
  const bool first = mq101_demo_next_ == 1;
  ++mq101_demo_next_;

  // Drop the next waypoint a few meters ahead, heading toward the quest's real
  // current-objective location when one is known (so the player walks the actual
  // route to the keep / exit), else along the player's facing.
  Vec3 ppos{};
  actors_->PlayerWorldPos(&ppos);
  Vec3 fwd{std::sin(ctx_.cam_yaw), 0.0f, -std::cos(ctx_.cam_yaw)};
  Vec3 target;
  if (quest_->CurrentObjectiveTarget(&target)) {
    const Vec3 to{target.x - ppos.x, 0.0f, target.z - ppos.z};
    const f32 len = Length(to);
    if (len > 1.0f) fwd = to * (1.0f / len);
  }
  QuestMarker m;
  m.quest = mq101_demo_quest_;
  m.advance_stage = advance_to;
  m.always_arm = true;  // the demo owns its waypoints, independent of journal timing
  m.pos = ppos + fwd * 6.0f;
  quest_->markers().push_back(m);

  // Recruit the nearest few NPCs as companions when the walk begins, so the
  // follow AI leads the cell's actual actors. Nearest-first (not a fixed radius)
  // so a populated cell like the Helgen keep always yields company.
  int recruited = 0;
  if (first) {
    struct Cand {
      u64 form;
      f32 dist_sq;
    };
    std::vector<Cand> cands;
    world_.Each<world::Npc, world::FormLink, world::Transform>(
        [&](ecs::Entity, world::Npc&, world::FormLink& link, world::Transform& t) {
          const f32 dx = t.position[0] - ppos.x, dz = t.position[2] - ppos.z;
          cands.push_back({link.form.packed(), dx * dx + dz * dz});
        });
    std::sort(cands.begin(), cands.end(),
              [](const Cand& a, const Cand& b) { return a.dist_sq < b.dist_sq; });
    for (size_t i = 0; i < cands.size() && recruited < 3; ++i) {
      if (cands[i].dist_sq > 60.0f * 60.0f) break;  // skip actors across the whole cell
      SetFollower(cands[i].form, true);
      ++recruited;
    }
  }

  REC_INFO("demo: MQ101 waypoint dropped -> reaching it advances to stage {}{}", advance_to,
           first ? Fmt(", %d companion(s) recruited", recruited) : std::string());
}

void NpcDirector::Mq101Sink::GuideTo(u64 actor, const float pos[3]) {
  const Vec3 target{pos[0], pos[1], pos[2]};
  if (Vec3* g = d->guides_.find(actor))
    *g = target;
  else
    d->guides_.insert(actor, target);
}

void NpcDirector::Mq101Sink::SayInfo(u64 /*actor*/, u64 info) { d->interaction_->RunInfoFragment(info); }

void NpcDirector::Mq101Sink::SetStage(u64 quest, i32 stage) {
  if (!d->ctx_.scripts || !d->ctx_.bindings) return;
  auto* binds = d->ctx_.bindings;
  d->ctx_.scripts->guest().Submit([binds, quest, stage](script::papyrus::VirtualMachine&) {
    binds->SetStage(script::papyrus::ObjectRef{quest}, stage);
  });
}

bool NpcDirector::Mq101Sink::ActorAt(u64 actor, const float pos[3], float radius) {
  bool found = false, reached = false;
  d->world_.Each<world::FormLink, world::Transform>(
      [&](ecs::Entity, world::FormLink& link, world::Transform& t) {
        if (link.form.packed() != actor) return;
        found = true;
        const float dx = t.position[0] - pos[0], dz = t.position[2] - pos[2];
        reached = dx * dx + dz * dz <= radius * radius;
      });
  return !found || reached;  // a streamed-out actor must not stall the scene
}

bool NpcDirector::Mq101Sink::PlayerNear(const float pos[3], float radius) {
  Vec3 p;
  if (!d->actors_->PlayerWorldPos(&p)) return true;
  const float dx = p.x - pos[0], dz = p.z - pos[2];
  return dx * dx + dz * dz <= radius * radius;
}

void NpcDirector::ArmMq101Scene(u64 quest_handle) {
  // Start the quest right away (asynchronously on the guest, like the demo) so
  // its opening fragment relocates the player to the real Helgen start; the
  // escort then waits for NPCs to stream in there before picking a guide. The
  // guest runs this once the player exists, so the MoveTo lands on the avatar.
  mq101_scene_quest_ = quest_handle;
  auto* binds = ctx_.bindings;
  ctx_.scripts->guest().Submit([binds, quest_handle](script::papyrus::VirtualMachine&) {
    script::papyrus::ObjectRef ref{quest_handle};
    binds->StartQuest(ref);
    binds->SetStage(ref, 160);  // "Make your way to the Keep"
  });
  mq101_scene_pending_ = true;
}

bool NpcDirector::StartMq101Scene() {
  if (mq101_scene_quest_ == 0 || !actors_->HasPlayer() || !ctx_.scripts || !ctx_.bindings)
    return false;
  const u64 mq = mq101_scene_quest_;

  Vec3 ppos{};
  if (!actors_->PlayerWorldPos(&ppos)) return false;
  // Pick the nearest NPC within reach as the guide who leads the escort. The
  // companion who shares the player's start (Ralof / Hadvar in the keep) is the
  // closest actor, so nearest-within-radius resolves to the right one without
  // hard-coding a form id.
  u64 guide = 0;
  float best = 30.0f * 30.0f;  // ignore actors across the cell
  world_.Each<world::Npc, world::FormLink, world::Transform>(
      [&](ecs::Entity, world::Npc&, world::FormLink& link, world::Transform& t) {
        const float dx = t.position[0] - ppos.x, dz = t.position[2] - ppos.z;
        const float d = dx * dx + dz * dz;
        if (d < best) {
          best = d;
          guide = link.form.packed();
        }
      });
  if (guide == 0) return false;  // NPCs not streamed in yet, retry

  // The guide leads to each mark, then the stage advances only once the player
  // has caught up: the player's own movement drives the journal, with the guide
  // showing the way. Marks step forward from the start along the player's
  // facing and are dropped onto the floor so they sit where feet can reach.
  const Vec3 fwd{std::sin(ctx_.cam_yaw), 0.0f, -std::cos(ctx_.cam_yaw)};
  mq101_scene_.actions.clear();
  auto add = [&](quest::SceneAction a) { mq101_scene_.actions.push_back(a); };
  auto set_stage = [&](i32 s) {
    quest::SceneAction a;
    a.kind = quest::SceneAction::Kind::kSetStage;
    a.quest = mq;
    a.stage = s;
    add(a);
  };
  auto lead_to = [&](const Vec3& p, i32 stage) {
    Vec3 mark{p.x, GroundY(p.x, p.z, p.y), p.z};
    quest::SceneAction guide_to;
    guide_to.kind = quest::SceneAction::Kind::kGuideTo;
    guide_to.actor = guide;
    guide_to.pos[0] = mark.x;
    guide_to.pos[1] = mark.y;
    guide_to.pos[2] = mark.z;
    guide_to.radius = 2.5f;
    add(guide_to);
    quest::SceneAction wait;
    wait.kind = quest::SceneAction::Kind::kWaitPlayerNear;
    wait.pos[0] = mark.x;
    wait.pos[1] = mark.y;
    wait.pos[2] = mark.z;
    wait.radius = 3.0f;
    add(wait);
    set_stage(stage);
  };

  // Stage 160 was set when the escort armed (it relocated the player here); the
  // guide now leads the remaining beats to completion.
  const i32 stages[4] = {300, 500, 700, 900};
  for (int i = 0; i < 4; ++i) lead_to(ppos + fwd * (8.0f * static_cast<f32>(i + 1)), stages[i]);

  scene_sink_.d = this;
  scene_runner_.Reset(&mq101_scene_);
  mq101_scene_active_ = true;
  mq101_scene_stuck_time_ = 0;
  REC_INFO("scene: MQ101 escort armed, guide 0x{:x} leads the player to completion", guide);
  return true;
}

void NpcDirector::Mq101SceneTick(f32 dt) {
  if (mq101_scene_pending_) {
    if (StartMq101Scene()) mq101_scene_pending_ = false;
    return;
  }
  if (!mq101_scene_active_) return;

  const size_t before = scene_runner_.current_action();
  const bool running = scene_runner_.Tick(scene_sink_, dt);
  // Stall recovery so the guided playthrough always finishes even on terrain a
  // guide wedges on or a player who cannot reach a mark: after a beat has made
  // no progress for a while, nudge whoever the beat is waiting on to the mark. A
  // kGuideTo waits on the guide; a kWaitPlayerNear waits on the player.
  constexpr f32 kStallSeconds = 20.0f;
  if (scene_runner_.current_action() != before) {
    mq101_scene_stuck_time_ = 0;
  } else if ((mq101_scene_stuck_time_ += dt) > kStallSeconds &&
             before < mq101_scene_.actions.size()) {
    const quest::SceneAction& a = mq101_scene_.actions[before];
    if (a.kind == quest::SceneAction::Kind::kGuideTo) {
      const float wx = a.pos[0], wy = a.pos[1], wz = a.pos[2];
      world_.Each<world::FormLink, world::Transform>(
          [&](ecs::Entity, world::FormLink& link, world::Transform& t) {
            if (link.form.packed() != a.actor) return;
            t.position[0] = wx;
            t.position[1] = wy;
            t.position[2] = wz;
          });
      REC_INFO("scene: guide reached the next mark");
    } else if (a.kind == quest::SceneAction::Kind::kWaitPlayerNear) {
      actors_->TeleportPlayer(a.pos[0], a.pos[1], a.pos[2]);
      REC_INFO("scene: player caught up to the next mark");
    }
    mq101_scene_stuck_time_ = 0;
  }
  // Steer the guided player toward the beat's mark (where the guide is leading).
  const size_t cur = scene_runner_.current_action();
  if (running && cur < mq101_scene_.actions.size()) {
    const quest::SceneAction& a = mq101_scene_.actions[cur];
    if (a.kind == quest::SceneAction::Kind::kGuideTo ||
        a.kind == quest::SceneAction::Kind::kWaitPlayerNear) {
      ctx_.auto_walk_goal = Vec3{a.pos[0], a.pos[1], a.pos[2]};
      ctx_.auto_walk_has_goal = true;
    }
  }
  if (!running) {
    mq101_scene_active_ = false;
    guides_.clear();
    ctx_.auto_walk_has_goal = false;
    REC_INFO("scene: MQ101 escort complete, quest driven to its completion stage");
  }
}

}  // namespace rec
