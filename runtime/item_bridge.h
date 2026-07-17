#ifndef RECREATION_RUNTIME_ITEM_BRIDGE_H_
#define RECREATION_RUNTIME_ITEM_BRIDGE_H_

#include <string>
#include <unordered_map>
#include <unordered_set>

#include "bethesda/form_id.h"
#include "core/math.h"
#include "core/types.h"
#include "ecs/entity.h"
#include "inventory/item_catalog.h"
#include "inventory/world_item.h"

namespace rx {

struct EngineContext;
class ActorSystem;

// Bridges the game's Bethesda item records onto rx::inventory: it lazily builds
// an rx ItemCatalog from WEAP/MISC/BOOK/INGR/ALCH/AMMO/... base records (display
// name, world mesh via the cell streamer's model pipeline, weight/value/flags,
// and a physics box shape from the mesh bounds), lets the player pick loose item
// references up into a real Inventory (removing the world ref *persistently*),
// drops the most recent stack back into the 3D world as a physics body, keeps
// the dropped-item field cheap via the rx hibernation store, and persists the
// whole lot (inventory + live/dormant world items + the removed-ref set) to a
// per-profile binary that reloads on the next session.
//
// Single place the game owns all item state; the engine loop calls Update() each
// frame and Save()/Load() at the session boundaries.
class ItemBridge {
 public:
  ItemBridge(EngineContext& ctx, ActorSystem* actors);

  // True when this base record is a pickup-able item (has a value/weight and a
  // world model the drop path can render), so the interaction layer can pick the
  // "Take" verb and route activation here.
  bool IsItemBase(bethesda::GlobalFormId base) const;

  // Activation handler: if `ref_handle` is a loose item reference, adds it to the
  // player's inventory, removes the world reference persistently, toasts, and
  // returns true. Returns false (does nothing) when the ref is not an item, so
  // the caller falls through to its other affordances. Host / single-player
  // authoritative (a client routes activation to the server, which calls this).
  bool TryPickUp(u64 ref_handle);

  // Drops the most recently added inventory stack in front of the player as a
  // dynamic physics body with a small forward toss. No-op when the inventory is
  // empty or no player exists. Bound to the drop keybind.
  void DropLast();

  // Per-frame maintenance keyed on the player position: mirrors awake body
  // transforms into ECS transforms, hibernates settled items beyond the far
  // radius into the spatial store, wakes stored items back near the player, and
  // periodically autosaves. Cheap once the loot field has settled.
  void Update(f32 dt);

  // Attaches the player's Inventory (+ stable Guid so save/load reattaches) and
  // loads any persisted state. Call once, after the player entity exists.
  void OnPlayerReady();

  // Writes inventory + world items + removed refs to the per-profile binary.
  void Save() const;
  // Returns the suppression predicate the cell streamer consults so a picked-up
  // reference is never re-placed after it streams out and back in.
  const std::unordered_set<u64>& removed_refs() const { return removed_refs_; }

  // Read-only view of the item catalog, so the first-person equip layer can map
  // an inventory ItemDefId to its def (WEAP flag + hand mesh). The catalog stays
  // owned here; callers only read it.
  const inventory::ItemCatalog& catalog() const { return catalog_; }

 private:
  // Resolves (building + caching on first use) the ItemDef id for an item base
  // form. kInvalidItemDef when the base is not a usable item. Registers the def
  // in the catalog under a stable id recorded in def_to_base_ for persistence.
  inventory::ItemDefId DefForBase(bethesda::GlobalFormId base);
  // Builds an ItemDef from a base record: name hash, world mesh + box shape via
  // the streamer, weight/value into weight/payload, mass from weight. Returns
  // false when the base carries no usable item data.
  bool BuildDef(bethesda::GlobalFormId base, inventory::ItemDef* out);
  // The player's Inventory component entity (creates the component lazily).
  ecs::Entity PlayerInventoryEntity();
  // Localized display name (FULL) of a record, empty when it has none.
  std::string RecordNameFor(bethesda::GlobalFormId id) const;

  std::string SavePath() const;
  bool Load();

  EngineContext& ctx_;
  ActorSystem* actors_;

  inventory::ItemCatalog catalog_;
  inventory::WorldItemStore world_store_;
  // Item base form (packed) -> catalog def id, and the reverse, so a saved def
  // id rebinds to the same base record across sessions.
  std::unordered_map<u64, inventory::ItemDefId> base_to_def_;
  std::unordered_map<inventory::ItemDefId, bethesda::GlobalFormId> def_to_base_;
  inventory::ItemDefId next_def_id_ = 1;
  // Loose item references the player has taken; the cell streamer skips these so
  // they stay gone across streaming and reloads.
  std::unordered_set<u64> removed_refs_;

  bool loaded_ = false;      // persisted state applied this session
  f32 autosave_timer_ = 0;   // seconds since the last autosave

  // RX_ITEM_PROBE debug harness: once, a few seconds after the world settles,
  // pick up the nearest loose item and drop it, logging each step so a headless
  // run exercises the full catalog/pickup/removal/drop/persist path without input
  // injection. No-op unless the env var is set.
  void MaybeRunProbe(f32 dt);
  f32 probe_timer_ = 0;
  bool probe_done_ = false;
};

}  // namespace rx

#endif  // RECREATION_RUNTIME_ITEM_BRIDGE_H_
