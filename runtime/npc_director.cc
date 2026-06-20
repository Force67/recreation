#include "npc_director.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "actor_system.h"
#include "core/log.h"
#include "core/math.h"
#include "engine_internal.h"
#include "interaction_system.h"
#include "quest_director.h"
#include "script/papyrus/value.h"
#include "world/components.h"
#include "world/npc_ai.h"
#include "world/pathfind.h"
#include "world/steering_avoidance.h"

namespace rec {

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
    const float spd = __builtin_sqrtf(out.velocity[0] * out.velocity[0] +
                                       out.velocity[2] * out.velocity[2]);
    const float gx = goal[0] - self_pos[0], gz = goal[2] - self_pos[2];
    const float gl = __builtin_sqrtf(gx * gx + gz * gz);
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

void NpcDirector::UpdateFollowers(f32 dt) {
  // Host authoritative: a client receives follower motion via actor sync.
  if (followers_.empty() || !actors_->HasPlayer() || ctx_.client_session) return;
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
  constexpr int kN = 15;       // odd so the NPC sits at the centre cell
  constexpr f32 kCell = 0.7f;  // meters per cell (~10 m grid span)
  const int c = kN / 2;
  const Vec3 origin{from.x - static_cast<f32>(c) * kCell, from.y,
                    from.z - static_cast<f32>(c) * kCell};  // grid (0,0) world
  auto cell_world = [&](int gx, int gy) {
    return Vec3{origin.x + static_cast<f32>(gx) * kCell, from.y,
                origin.z + static_cast<f32>(gy) * kCell};
  };
  // A cell is blocked when a downward ray from above it finds no floor at a
  // walkable height: a void / ledge (no hit, or floor far below) or a wall
  // footprint (the ray hits the wall top / interior, well above the NPC's feet).
  // Downward rays read floors cleanly, unlike horizontal rays grazing terrain.
  bool grid[kN * kN];
  for (int gy = 0; gy < kN; ++gy)
    for (int gx = 0; gx < kN; ++gx) {
      if (gx == c && gy == c) {
        grid[gy * kN + gx] = false;  // the NPC's own cell
        continue;
      }
      const Vec3 w = cell_world(gx, gy);
      physics::PhysicsWorld::RayHit hit;
      const bool floor = physics_.Raycast(Vec3{w.x, from.y + 2.0f, w.z}, Vec3{0, -1, 0}, 4.0f, &hit);
      grid[gy * kN + gx] = !floor || std::fabs(hit.position.y - from.y) > 0.6f;
    }
  auto blocked = [&](int gx, int gy) -> bool { return grid[gy * kN + gx]; };
  const world::PathNode start{c, c};
  int ggx = std::clamp(static_cast<int>(std::lround((goal.x - origin.x) / kCell)), 0, kN - 1);
  int ggy = std::clamp(static_cast<int>(std::lround((goal.z - origin.z) / kCell)), 0, kN - 1);
  std::vector<world::PathNode> path;
  if (!world::FindPath(kN, kN, blocked, start, {ggx, ggy}, &path, kN * kN) || path.size() < 2)
    return goal;  // no route: head straight and let local avoidance cope
  // Aim a few cells ahead along the path for smooth motion, not the next cell.
  const world::PathNode& step = path[std::min<size_t>(3, path.size() - 1)];
  const Vec3 w = cell_world(step.x, step.y);
  return Vec3{w.x, goal.y, w.z};
}

void NpcDirector::UpdateGuides(f32 dt) {
  // Host authoritative: a client receives guide motion via actor sync.
  if (guides_.empty() || ctx_.client_session) return;
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
    REC_INFO("demo: MQ101 breadcrumb finished, quest driven to its completion stage");
    return;
  }

  const i32 advance_to = mq101_demo_stages_[mq101_demo_next_];
  const bool first = mq101_demo_next_ == 1;
  ++mq101_demo_next_;

  // Drop the next waypoint a few meters ahead along the player's facing.
  Vec3 ppos{};
  actors_->PlayerWorldPos(&ppos);
  const Vec3 fwd{std::sin(ctx_.cam_yaw), 0.0f, -std::cos(ctx_.cam_yaw)};
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
  if (!running) {
    mq101_scene_active_ = false;
    guides_.clear();
    REC_INFO("scene: MQ101 escort complete, quest driven to its completion stage");
  }
}

}  // namespace rec
