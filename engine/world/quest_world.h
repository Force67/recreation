#ifndef RECREATION_WORLD_QUEST_WORLD_H_
#define RECREATION_WORLD_QUEST_WORLD_H_

#include <array>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "asset/asset_id.h"
#include "core/types.h"
#include "ecs/entity.h"

namespace rx::ecs {
class World;
}

namespace rx::world {

// A world mutation a quest asked for, marshaled from the Papyrus guest thread to
// the main thread (which owns the ECS). Every command carries the issuing quest
// so QuestWorld can record provenance and later roll it back.
enum class WorldOp : u8 {
  kSpawn,         // create an entity (PlaceAtMe)
  kMove,          // set an entity's position (MoveTo / SetPosition)
  kSetEnabled,    // Enable/Disable: toggle the Hidden tag
  kDelete,        // destroy an entity
  kMovePlayer,    // teleport the player actor (applied by a host hook)
  kCleanupQuest,  // undo everything a quest created or changed
};

struct WorldCommand {
  WorldOp op = WorldOp::kMove;
  u64 quest = 0;   // issuing quest handle (0 = not attributed, not rolled back)
  u64 handle = 0;  // target form handle (the placed/spawned ref)
  u64 base = 0;    // base form for kSpawn
  asset::AssetId mesh;     // resolved mesh for kSpawn
  bool has_mesh = false;   // whether `mesh` is valid (spawn is drawn)
  bool is_actor = false;   // kSpawn: tag the entity Npc so it renders as a biped
                           // and the actor sync streams its movement (battle soldiers)
  i32 team = 0;            // kSpawn actor: combat side, replicated so a client renders
                           // the matching faction armour/tint (0 = none, 1/2 = sides)
  std::array<f32, 3> pos{0, 0, 0};
  std::array<f32, 4> rot{0, 0, 0, 1};
  f32 scale = 1.0f;
  bool enabled = true;
};

// Thread-safe command buffer: the guest thread pushes, the main thread drains.
class WorldCommandQueue {
 public:
  void Push(const WorldCommand& cmd);
  std::vector<WorldCommand> Drain();  // moves out all queued commands

 private:
  std::mutex mutex_;
  std::vector<WorldCommand> commands_;
};

// Applies quest world commands to the ECS and records per-quest provenance, so a
// quest's world effects can be undone wholesale (CleanupQuest), the explicit
// goal being that quest-created state never leaks. Lives on the main thread with
// the ECS world.
class QuestWorld {
 public:
  explicit QuestWorld(ecs::World& world) : world_(world) {}

  // The player is not a registry entity; quests that MoveTo the player route
  // through this hook (the runtime teleports the player actor/capsule).
  void set_player_handle(u64 handle) { player_handle_ = handle; }
  // `dest_ref` is the reference the player was moved to (0 = raw coordinates),
  // so the runtime can switch cells when it names an interior.
  void set_on_move_player(std::function<void(u64 dest_ref, f32, f32, f32)> fn) {
    on_move_player_ = std::move(fn);
  }

  // Fires when a tracked reference unloads (Unregister), so the runtime can raise
  // a managed FormUnloaded event symmetric to the scripts-attached FormLoaded.
  void set_on_unregister(std::function<void(u64 handle)> fn) {
    on_unregister_ = std::move(fn);
  }

  // Registers a pre-existing reference's entity (e.g. a cell-streamed REFR) so
  // quests can Move/Disable/Delete it. Spawned refs register themselves.
  void Register(u64 handle, ecs::Entity entity);
  void Unregister(u64 handle);
  ecs::Entity Find(u64 handle) const;  // kInvalidEntity if unknown

  // Fills `out` with (handle, world-space position) for every registered
  // reference that has a transform. The runtime snapshots these each frame for
  // the managed proximity query, which reads the snapshot off the guest thread.
  void SnapshotPositions(std::vector<std::pair<u64, std::array<f32, 3>>>& out) const;

  // Drains the queue and applies each command, recording provenance.
  void Apply(WorldCommandQueue& queue);

  // Applies an explicit command list. The host drains its queue, replicates the
  // list to clients, and applies it here; a client applies the list it received.
  // Same code path both sides, so provenance/cleanup stays identical everywhere.
  void Apply(const std::vector<WorldCommand>& commands);

  // Undoes everything quest `quest` created or changed, newest first.
  void CleanupQuest(u64 quest);

  size_t tracked_entities() const { return registry_.size(); }
  size_t quests_with_effects() const { return provenance_.size(); }

 private:
  enum class EffectKind : u8 { kSpawned, kMoved, kEnabledChanged };
  struct Effect {
    EffectKind kind;
    u64 handle = 0;
    std::array<f32, 3> prev_pos{0, 0, 0};  // kMoved: position before the move
    bool prev_hidden = false;              // kEnabledChanged: Hidden before toggle
  };

  void ApplyOne(const WorldCommand& cmd);
  void RecordEffect(u64 quest, const Effect& effect);

  ecs::World& world_;
  std::unordered_map<u64, ecs::Entity> registry_;
  std::unordered_map<u64, std::vector<Effect>> provenance_;
  std::function<void(u64, f32, f32, f32)> on_move_player_;
  std::function<void(u64)> on_unregister_;
  u64 player_handle_ = 0x14;
};

}  // namespace rx::world

#endif  // RECREATION_WORLD_QUEST_WORLD_H_
