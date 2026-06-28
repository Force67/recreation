#include "world/quest_world.h"

#include "bethesda/form_id.h"
#include "core/log.h"
#include "ecs/world.h"
#include "world/components.h"

namespace rec::world {

void WorldCommandQueue::Push(const WorldCommand& cmd) {
  std::lock_guard<std::mutex> lock(mutex_);
  commands_.push_back(cmd);
}

std::vector<WorldCommand> WorldCommandQueue::Drain() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<WorldCommand> out;
  out.swap(commands_);
  return out;
}

void QuestWorld::Register(u64 handle, ecs::Entity entity) { registry_[handle] = entity; }
void QuestWorld::Unregister(u64 handle) {
  if (registry_.erase(handle) != 0 && on_unregister_) on_unregister_(handle);
}

ecs::Entity QuestWorld::Find(u64 handle) const {
  auto it = registry_.find(handle);
  return it == registry_.end() ? ecs::kInvalidEntity : it->second;
}

void QuestWorld::RecordEffect(u64 quest, const Effect& effect) {
  if (quest != 0) provenance_[quest].push_back(effect);
}

void QuestWorld::Apply(WorldCommandQueue& queue) { Apply(queue.Drain()); }

void QuestWorld::Apply(const std::vector<WorldCommand>& commands) {
  for (const WorldCommand& cmd : commands) ApplyOne(cmd);
}

void QuestWorld::ApplyOne(const WorldCommand& cmd) {
  switch (cmd.op) {
    case WorldOp::kSpawn: {
      // Idempotent: a re-sent spawn (e.g. a battle resync to a late-joining
      // client) must not duplicate an entity that already exists for this handle.
      if (cmd.handle != 0 && registry_.find(cmd.handle) != registry_.end()) break;
      ecs::Entity entity = world_.Create();
      Transform t;
      for (int i = 0; i < 3; ++i) t.position[i] = cmd.pos[i];
      for (int i = 0; i < 4; ++i) t.rotation[i] = cmd.rot[i];
      t.scale = cmd.scale;
      world_.Add(entity, t);
      world_.Add(entity, FormLink{bethesda::GlobalFormId{static_cast<u16>(cmd.handle >> 32),
                                                         static_cast<u32>(cmd.handle)}});
      world_.Add(entity, QuestSpawned{cmd.quest});
      if (cmd.has_mesh) world_.Add(entity, Renderable{cmd.mesh});
      // A replicated actor spawn (battle soldier): tag it Npc so the actor system
      // renders a biped and the actor sync recognises it (host and client agree
      // on the entity by its form handle).
      if (cmd.is_actor) {
        world_.Add(entity, Npc{bethesda::GlobalFormId{static_cast<u16>(cmd.base >> 32),
                                                      static_cast<u32>(cmd.base)}});
        REC_INFO("quest_world: spawned replicated actor 0x{:x}", cmd.handle);
      }
      registry_[cmd.handle] = entity;
      RecordEffect(cmd.quest, {EffectKind::kSpawned, cmd.handle, {}, false});
      break;
    }
    case WorldOp::kMove: {
      ecs::Entity entity = Find(cmd.handle);
      if (!world_.IsAlive(entity)) break;
      Transform* t = world_.Get<Transform>(entity);
      if (!t) break;
      RecordEffect(cmd.quest, {EffectKind::kMoved,
                               cmd.handle,
                               {t->position[0], t->position[1], t->position[2]},
                               false});
      for (int i = 0; i < 3; ++i) t->position[i] = cmd.pos[i];
      break;
    }
    case WorldOp::kSetEnabled: {
      ecs::Entity entity = Find(cmd.handle);
      if (!world_.IsAlive(entity)) break;
      const bool was_hidden = world_.Has<Hidden>(entity);
      RecordEffect(cmd.quest, {EffectKind::kEnabledChanged, cmd.handle, {}, was_hidden});
      if (cmd.enabled && was_hidden) world_.Remove<Hidden>(entity);
      if (!cmd.enabled && !was_hidden) world_.Add(entity, Hidden{});
      break;
    }
    case WorldOp::kDelete: {
      ecs::Entity entity = Find(cmd.handle);
      if (world_.IsAlive(entity)) world_.Destroy(entity);
      registry_.erase(cmd.handle);
      break;
    }
    case WorldOp::kMovePlayer:
      if (on_move_player_) on_move_player_(cmd.handle, cmd.pos[0], cmd.pos[1], cmd.pos[2]);
      break;
    case WorldOp::kCleanupQuest:
      CleanupQuest(cmd.quest);
      break;
  }
}

void QuestWorld::CleanupQuest(u64 quest) {
  auto it = provenance_.find(quest);
  if (it == provenance_.end()) return;
  std::vector<Effect>& effects = it->second;
  // Undo newest first so a move recorded after a spawn is reverted before the
  // spawn is destroyed (and a destroyed entity is simply skipped).
  for (auto e = effects.rbegin(); e != effects.rend(); ++e) {
    ecs::Entity entity = Find(e->handle);
    switch (e->kind) {
      case EffectKind::kSpawned:
        if (world_.IsAlive(entity)) world_.Destroy(entity);
        registry_.erase(e->handle);
        break;
      case EffectKind::kMoved:
        if (world_.IsAlive(entity))
          if (Transform* t = world_.Get<Transform>(entity))
            for (int i = 0; i < 3; ++i) t->position[i] = e->prev_pos[i];
        break;
      case EffectKind::kEnabledChanged:
        if (world_.IsAlive(entity)) {
          const bool hidden = world_.Has<Hidden>(entity);
          if (e->prev_hidden && !hidden) world_.Add(entity, Hidden{});
          if (!e->prev_hidden && hidden) world_.Remove<Hidden>(entity);
        }
        break;
    }
  }
  provenance_.erase(it);
}

void QuestWorld::SnapshotPositions(
    std::vector<std::pair<u64, std::array<f32, 3>>>& out) const {
  out.clear();
  out.reserve(registry_.size());
  for (const auto& [handle, entity] : registry_) {
    if (const Transform* t = world_.Get<Transform>(entity))
      out.push_back({handle, {t->position[0], t->position[1], t->position[2]}});
  }
}

}  // namespace rec::world
