#include "world/quest_world.h"

#include <algorithm>
#include <cmath>

#include "bethesda/form_id.h"
#include "core/log.h"
#include "ecs/world.h"
#include "world/components.h"

namespace rx::world {

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

void QuestWorld::Register(u64 handle, ecs::Entity entity) {
  registry_[handle] = entity;
  if (world_.IsAlive(entity)) {
    DoorState* door = world_.Get<DoorState>(entity);
    auto override = door_overrides_.find(handle);
    if (door && override != door_overrides_.end()) {
      if (override->second.locked) door->locked = *override->second.locked;
      if (override->second.open) door->open = *override->second.open;
      if (on_door_state_) on_door_state_(handle, door->locked, door->open);
    }
  }
  auto pending = pending_.find(handle);
  if (pending != pending_.end()) {
    std::vector<WorldCommand> commands = std::move(pending->second);
    pending_.erase(pending);
    pending_overflow_warned_.erase(handle);
    for (const WorldCommand& command : commands) ApplyOne(command);
  }
  if (on_register_ && world_.IsAlive(entity) && !world_.Has<Deleted>(entity)) on_register_(handle);
}
void QuestWorld::Unregister(u64 handle) {
  auto it = registry_.find(handle);
  if (it == registry_.end()) return;
  const bool already_unloaded = world_.IsAlive(it->second) && world_.Has<Deleted>(it->second);
  registry_.erase(it);
  if (!already_unloaded && on_unregister_) on_unregister_(handle);
}

ecs::Entity QuestWorld::Find(u64 handle) const {
  auto it = registry_.find(handle);
  return it == registry_.end() ? ecs::kInvalidEntity : it->second;
}

bool QuestWorld::CanActivateFrom(ecs::Entity player, u64 handle, f32 max_distance) const {
  const ecs::Entity target = Find(handle);
  if (max_distance <= 0.0f || !world_.IsAlive(player) || !world_.IsAlive(target) ||
      world_.Has<Hidden>(player) || world_.Has<Deleted>(player) || world_.Has<Hidden>(target) ||
      world_.Has<Deleted>(target)) {
    return false;
  }
  const Transform* player_transform = world_.Get<Transform>(player);
  const Transform* target_transform = world_.Get<Transform>(target);
  if (!player_transform || !target_transform) return false;
  f32 distance_sq = 0.0f;
  for (u32 axis = 0; axis < 3; ++axis) {
    const f32 delta = target_transform->position[axis] - player_transform->position[axis];
    distance_sq += delta * delta;
  }
  return std::isfinite(distance_sq) && distance_sq <= max_distance * max_distance;
}

void QuestWorld::RecordEffect(u64 quest, Effect effect) {
  if (quest == 0) return;
  effect.sequence = next_effect_sequence_++;
  provenance_[quest].push_back(std::move(effect));
}

void QuestWorld::DeferCommand(const WorldCommand& command) {
  std::vector<WorldCommand>& commands = pending_[command.handle];
  const bool coalescible = command.op == WorldOp::kMove || command.op == WorldOp::kSetEnabled ||
                           command.op == WorldOp::kSetLocked || command.op == WorldOp::kSetOpen ||
                           command.op == WorldOp::kDelete;
  if (coalescible && !commands.empty()) {
    WorldCommand& last = commands.back();
    if (last.op == command.op && last.quest == command.quest) {
      last = command;
      return;
    }
  }
  if (commands.size() == kMaxPendingCommandsPerHandle) {
    if (!pending_overflow_warned_[command.handle]) {
      RX_WARN("quest_world: deferred command limit reached for 0x{:x}; dropping oldest",
              command.handle);
      pending_overflow_warned_[command.handle] = true;
    }
    commands.erase(commands.begin());
  }
  commands.push_back(command);
}

void QuestWorld::Apply(WorldCommandQueue& queue) { Apply(queue.Drain()); }

void QuestWorld::Apply(const std::vector<WorldCommand>& commands) {
  for (const WorldCommand& cmd : commands) ApplyOne(cmd);
}

void QuestWorld::ApplyOne(const WorldCommand& cmd) {
  switch (cmd.op) {
    case WorldOp::kMove:
    case WorldOp::kSetEnabled:
    case WorldOp::kSetLocked:
    case WorldOp::kSetOpen:
    case WorldOp::kDelete:
      if (!world_.IsAlive(Find(cmd.handle))) {
        DeferCommand(cmd);
        return;
      }
      break;
    default:
      break;
  }

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
        // Carry the combat side for rendering only (the client does not simulate
        // combat) so the actor system instances the matching faction armour.
        if (cmd.team != 0) world_.Add(entity, CombatTeam{cmd.team});
        RX_INFO("quest_world: spawned replicated actor 0x{:x} (team {})", cmd.handle, cmd.team);
      }
      registry_[cmd.handle] = entity;
      RecordEffect(cmd.quest, {EffectKind::kSpawned, cmd.handle, {}, false});
      if (on_reference_changed_) on_reference_changed_(cmd.handle);
      break;
    }
    case WorldOp::kMove: {
      ecs::Entity entity = Find(cmd.handle);
      if (!world_.IsAlive(entity)) break;
      if (world_.Has<Deleted>(entity)) break;
      Transform* t = world_.Get<Transform>(entity);
      if (!t) break;
      RecordEffect(cmd.quest, {EffectKind::kMoved,
                               cmd.handle,
                               {t->position[0], t->position[1], t->position[2]},
                               false});
      for (int i = 0; i < 3; ++i) t->position[i] = cmd.pos[i];
      if (on_reference_changed_) on_reference_changed_(cmd.handle);
      break;
    }
    case WorldOp::kSetEnabled: {
      ecs::Entity entity = Find(cmd.handle);
      if (!world_.IsAlive(entity)) break;
      if (world_.Has<Deleted>(entity)) break;
      bool was_hidden = world_.Has<Hidden>(entity);
      bool hidden = !cmd.enabled;
      if (PackInOwner* owner = world_.Get<PackInOwner>(entity)) {
        was_hidden = owner->independently_hidden;
        owner->independently_hidden = !cmd.enabled;
        const ecs::Entity root = Find(owner->root);
        hidden |= !world_.IsAlive(root) || world_.Has<Hidden>(root) || world_.Has<Deleted>(root);
      }
      RecordEffect(cmd.quest, {EffectKind::kEnabledChanged, cmd.handle, {}, was_hidden});
      if (!hidden && world_.Has<Hidden>(entity)) world_.Remove<Hidden>(entity);
      if (hidden && !world_.Has<Hidden>(entity)) world_.Add(entity, Hidden{});
      if (on_reference_changed_) on_reference_changed_(cmd.handle);
      break;
    }
    case WorldOp::kSetLocked: {
      ecs::Entity entity = Find(cmd.handle);
      if (!world_.IsAlive(entity)) break;
      DoorState* door = world_.Get<DoorState>(entity);
      if (!door) break;
      Effect effect{EffectKind::kDoorLockedChanged, cmd.handle, {}, door->locked};
      effect.value = cmd.enabled;
      RecordEffect(cmd.quest, std::move(effect));
      door->locked = cmd.enabled;
      door_overrides_[cmd.handle].locked = door->locked;
      if (on_door_state_) on_door_state_(cmd.handle, door->locked, door->open);
      if (on_reference_changed_) on_reference_changed_(cmd.handle);
      break;
    }
    case WorldOp::kSetOpen: {
      ecs::Entity entity = Find(cmd.handle);
      if (!world_.IsAlive(entity)) break;
      DoorState* door = world_.Get<DoorState>(entity);
      if (!door) break;
      Effect effect{EffectKind::kDoorOpenChanged, cmd.handle, {}, door->open};
      effect.value = cmd.enabled;
      RecordEffect(cmd.quest, std::move(effect));
      door->open = cmd.enabled;
      door_overrides_[cmd.handle].open = door->open;
      if (on_door_state_) on_door_state_(cmd.handle, door->locked, door->open);
      if (on_reference_changed_) on_reference_changed_(cmd.handle);
      break;
    }
    case WorldOp::kDelete: {
      ecs::Entity entity = Find(cmd.handle);
      if (world_.IsAlive(entity) && world_.Has<Prop>(entity) &&
          world_.Has<CellMembership>(entity)) {
        if (!world_.Has<Hidden>(entity)) world_.Add(entity, Hidden{});
        if (!world_.Has<Deleted>(entity)) {
          world_.Add(entity, Deleted{});
          if (on_unregister_) on_unregister_(cmd.handle);
        }
      } else {
        if (world_.IsAlive(entity)) world_.Destroy(entity);
        const bool tracked = registry_.erase(cmd.handle) != 0;
        if (tracked && on_unregister_) on_unregister_(cmd.handle);
      }
      if (on_reference_changed_) on_reference_changed_(cmd.handle);
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
  for (auto it = pending_.begin(); it != pending_.end();) {
    std::vector<WorldCommand>& commands = it->second;
    commands.erase(
        std::remove_if(commands.begin(), commands.end(),
                       [quest](const WorldCommand& command) { return command.quest == quest; }),
        commands.end());
    if (commands.empty())
      it = pending_.erase(it);
    else
      ++it;
  }

  auto it = provenance_.find(quest);
  if (it == provenance_.end()) return;
  std::vector<Effect>& effects = it->second;
  struct DoorProperty {
    u64 handle;
    EffectKind kind;
    bool baseline;
  };
  std::vector<DoorProperty> door_properties;
  for (const Effect& removed : effects) {
    if (removed.kind != EffectKind::kDoorLockedChanged &&
        removed.kind != EffectKind::kDoorOpenChanged)
      continue;
    const bool already_tracked =
        std::any_of(door_properties.begin(), door_properties.end(), [&](const DoorProperty& p) {
          return p.handle == removed.handle && p.kind == removed.kind;
        });
    if (already_tracked) continue;

    const Effect* first = nullptr;
    for (const auto& [_, active_effects] : provenance_) {
      for (const Effect& active : active_effects) {
        if (active.handle != removed.handle || active.kind != removed.kind) continue;
        if (!first || active.sequence < first->sequence) first = &active;
      }
    }
    door_properties.push_back({removed.handle, removed.kind,
                               first ? first->prev_value : removed.prev_value});
  }
  // Undo newest first so a move recorded after a spawn is reverted before the
  // spawn is destroyed (and a destroyed entity is simply skipped).
  for (auto e = effects.rbegin(); e != effects.rend(); ++e) {
    ecs::Entity entity = Find(e->handle);
    switch (e->kind) {
      case EffectKind::kSpawned:
        if (world_.IsAlive(entity)) world_.Destroy(entity);
        if (registry_.erase(e->handle) != 0 && on_unregister_) on_unregister_(e->handle);
        break;
      case EffectKind::kMoved:
        if (world_.IsAlive(entity)) {
          if (Transform* t = world_.Get<Transform>(entity))
            for (int i = 0; i < 3; ++i) t->position[i] = e->prev_pos[i];
          if (on_reference_changed_) on_reference_changed_(e->handle);
        } else {
          WorldCommand restore;
          restore.op = WorldOp::kMove;
          restore.handle = e->handle;
          restore.pos = e->prev_pos;
          DeferCommand(restore);
        }
        break;
      case EffectKind::kEnabledChanged:
        if (world_.IsAlive(entity)) {
          if (world_.Has<Deleted>(entity)) break;
          bool hidden = e->prev_value;
          if (PackInOwner* owner = world_.Get<PackInOwner>(entity)) {
            owner->independently_hidden = e->prev_value;
            const ecs::Entity root = Find(owner->root);
            hidden |=
                !world_.IsAlive(root) || world_.Has<Hidden>(root) || world_.Has<Deleted>(root);
          }
          if (hidden && !world_.Has<Hidden>(entity)) world_.Add(entity, Hidden{});
          if (!hidden && world_.Has<Hidden>(entity)) world_.Remove<Hidden>(entity);
          if (on_reference_changed_) on_reference_changed_(e->handle);
        } else {
          WorldCommand restore;
          restore.op = WorldOp::kSetEnabled;
          restore.handle = e->handle;
          restore.enabled = !e->prev_value;
          DeferCommand(restore);
        }
        break;
      case EffectKind::kDoorLockedChanged:
      case EffectKind::kDoorOpenChanged:
        break;
    }
  }
  provenance_.erase(it);

  // Removing an older quest must not restore through newer surviving door
  // effects. Rebuild each affected property's predecessor chain from its stable
  // baseline, then apply the value implied by the latest survivor.
  for (const DoorProperty& property : door_properties) {
    std::vector<Effect*> surviving;
    for (auto& [_, active_effects] : provenance_)
      for (Effect& active : active_effects)
        if (active.handle == property.handle && active.kind == property.kind)
          surviving.push_back(&active);
    std::sort(surviving.begin(), surviving.end(), [](const Effect* a, const Effect* b) {
      return a->sequence < b->sequence;
    });

    bool value = property.baseline;
    for (Effect* active : surviving) {
      active->prev_value = value;
      value = active->value;
    }

    DoorOverride& override = door_overrides_[property.handle];
    if (property.kind == EffectKind::kDoorLockedChanged)
      override.locked = value;
    else
      override.open = value;

    const ecs::Entity entity = Find(property.handle);
    DoorState* door = world_.IsAlive(entity) ? world_.Get<DoorState>(entity) : nullptr;
    if (!door) continue;
    if (property.kind == EffectKind::kDoorLockedChanged)
      door->locked = value;
    else
      door->open = value;
    if (on_door_state_) on_door_state_(property.handle, door->locked, door->open);
    if (on_reference_changed_) on_reference_changed_(property.handle);
  }
}

void QuestWorld::SnapshotPositions(std::vector<std::pair<u64, std::array<f32, 3>>>& out) const {
  out.clear();
  out.reserve(registry_.size());
  for (const auto& [handle, entity] : registry_) {
    if (world_.Has<Hidden>(entity) || world_.Has<Deleted>(entity)) continue;
    if (const Transform* t = world_.Get<Transform>(entity))
      out.push_back({handle, {t->position[0], t->position[1], t->position[2]}});
  }
}

std::vector<WorldCommand> QuestWorld::SnapshotDoorStates() const {
  std::vector<WorldCommand> out;
  out.reserve(door_overrides_.size() * 4);
  auto append_state = [&](u64 handle, WorldOp op, bool current, EffectKind kind) {
    struct ActiveEffect {
      u64 quest;
      const Effect* effect;
    };
    std::vector<ActiveEffect> effects;
    for (const auto& [quest, quest_effects] : provenance_)
      for (const Effect& effect : quest_effects)
        if (effect.handle == handle && effect.kind == kind) effects.push_back({quest, &effect});
    std::sort(effects.begin(), effects.end(), [](const ActiveEffect& a, const ActiveEffect& b) {
      return a.effect->sequence < b.effect->sequence;
    });

    WorldCommand command;
    command.op = op;
    command.handle = handle;
    if (effects.empty()) {
      command.enabled = current;
      out.push_back(command);
      return;
    }

    // Seed the first still-active effect's recorded predecessor without adding
    // provenance, then replay each active effect in its original order. This
    // reconstructs the same cleanup ledger even after earlier quests were removed.
    command.enabled = effects.front().effect->prev_value;
    out.push_back(command);
    for (size_t i = 0; i < effects.size(); ++i) {
      command.quest = effects[i].quest;
      command.enabled = i + 1 < effects.size() ? effects[i + 1].effect->prev_value : current;
      out.push_back(command);
    }
  };
  for (const auto& [handle, state] : door_overrides_) {
    if (state.locked)
      append_state(handle, WorldOp::kSetLocked, *state.locked,
                   EffectKind::kDoorLockedChanged);
    if (state.open)
      append_state(handle, WorldOp::kSetOpen, *state.open, EffectKind::kDoorOpenChanged);
  }
  for (const auto& [handle, commands] : pending_) {
    for (const WorldCommand& command : commands) {
      if (command.op == WorldOp::kSetLocked || command.op == WorldOp::kSetOpen)
        out.push_back(command);
    }
  }
  return out;
}

size_t QuestWorld::pending_command_count() const {
  size_t count = 0;
  for (const auto& [_, commands] : pending_) count += commands.size();
  return count;
}

}  // namespace rx::world
