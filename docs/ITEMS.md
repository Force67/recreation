# Items: pickup, drop, and forever-persistence

Recreation lets the player take loose item references out of the world into a
real inventory, drop them back into the 3D world where they fall with physics,
and have both the inventory **and** the dropped items survive quit/reload and
cell streaming. The gameplay logic lives in the reusable `rx::inventory` /
`rx::inventory_world` engine modules; recreation supplies the Bethesda-specific
bridge and the persistence store.

## Pieces

- **`rx::inventory` (engine module).** Item catalog, stacking `Inventory` /
  `Equipment` components, `AddItem`/`RemoveItem`/`DropItem`/`PickUpItem`, the
  sleep-aware `SyncWorldItems`, the hibernation store (`WorldItemStore` +
  `HibernateDistantWorldItems`/`WakeWorldItemsNear`), and versioned binary
  serialization (`SaveInventories`/`SaveWorldItems`). The engine owns no item
  taxonomy — an `ItemDef` is just `{name_hash, world_mesh, shape, mass, weight,
  flags, payload}`.
- **`runtime/item_bridge.{h,cc}` (`ItemBridge`).** The game side: it lazily
  builds the `ItemCatalog` from Bethesda base records, routes activation into
  pickups, binds the drop keybind, runs the per-frame world-item upkeep, and owns
  the per-profile save file.
- **`CellStreamer` additions.** `PrepareItemModel(base, &render_id)` converts +
  uploads a base form's NIF through the same pipeline a placed ref uses (so a
  dropped item gets the identical renderer mesh) without spawning an entity, and
  `set_ref_suppressor(fn)` lets the streamer skip a persistently-removed ref.

## Catalog bridge

The first time the player interacts with an item form, `ItemBridge::DefForBase`
builds an `ItemDef`:
- **name** — FULL (localized via the string table), used for HUD toasts.
- **world mesh** — `CellStreamer::PrepareItemModel` (MODL → NIF → uploaded
  renderer mesh, salted for the domain). The dropped entity carries
  `scene::Renderable{world_mesh}` + a `scene::Transform` scaled by the
  unit→metre constant `0.01428`, so it renders through the existing frame draw
  pass exactly like a streamed static.
- **physics shape** — a box whose half extents enclose the mesh's lod-0 vertex
  AABB (symmetric about the origin, since item NIFs model around it), with the
  same `0.01428` shape-unit→metre scale. A box-from-bounds fallback is used
  throughout; authored NIF `bhk` collision is not wired into the drop path yet.
- **mass** — derived from the record weight (clamped 0.1–100 kg).
- **weight/value** — from the `DATA` subrecord; value goes into `ItemDef.payload`,
  the record signature into `ItemDef.flags`.

Defs are cached per base form and assigned a stable `ItemDefId` recorded in the
save so a saved inventory/world-item rebinds to the same record next session.

## Pickup and drop

- **Pickup.** Activating a loose item reference (verb shows "Take") calls
  `ItemBridge::TryPickUp`: it resolves the ref's base, adds it to the player's
  `Inventory`, destroys the world ref entity, and records the ref handle in the
  removed-ref set. Host / single-player authoritative — a multiplayer client
  routes activation to the server (the existing `kActivateRef` path), whose
  `RaiseActivate` calls `TryPickUp` (idempotent). MP replication of the removal
  to other clients is left for a later pass; single-player is complete.
- **Drop.** The `drop_item` action (default **G**, walk mode only) calls
  `ItemBridge::DropLast`, which `DropItem`s the most recent inventory stack ~0.6 m
  in front of the player with a small forward+up impulse; it lands and tumbles to
  rest as a dynamic Jolt body.

Both toast the player (`GameUi::FlashQuestUpdate`) and log an `item:` line.

## Persistence model (forever, performant)

**Store:** one per-profile binary at
`~/.config/recreation/profiles/<player_name>/items.bin` — a small `"RITM"`
versioned container holding (1) the def-id → base-form table, (2) the removed-ref
set, (3) the `rx::inventory` blob (`SaveInventories`, keyed by a stable
`scene::Guid` on the player so it reattaches across ECS handle reuse), and (4) the
`rx::inventory_world` blob (`SaveWorldItems`, live + hibernated items keyed by a
stable `persistent_id`).

**Why a custom store, not QuestWorld provenance.** `QuestWorld` provenance is
in-memory only and exists to roll a quest's effects back within a session — it
has no disk format. Removed item refs must persist *forever*, so they get their
own on-disk set, consulted by `CellStreamer::set_ref_suppressor` at the single
existing "don't place this ref" point in `SpawnReference` (beside the
initially-disabled flag). A ref the player took therefore never re-places when
its cell streams out and back in, or after a restart. On load, any ref already
streamed in before the suppressor was installed is swept and destroyed.

**Perf: eviction, not iteration.** The cost of "thousands of dropped items lie
there forever" is the live Jolt body and per-frame system iteration.
`ItemBridge::Update` (run each simulation frame after physics steps) does:
1. `SyncWorldItems` — mirror only *awake* bodies into ECS transforms; a settled
   item latches `at_rest` and is skipped thereafter (steady-state ≈ free).
2. `HibernateDistantWorldItems` (radius **256 m**) — evict at-rest items beyond
   the radius: destroy the body, drop the entity, keep a ~64-byte record in a
   cell-bucketed `WorldItemStore`.
3. `WakeWorldItemsNear` (radius **192 m**) — re-materialise stored items near the
   player. The wake radius is smaller than the hibernate radius (hysteresis) so a
   boundary item never thrashes.

The hibernate radius sits comfortably beyond the streaming bubble (load radius 3
cells ≈ 175 m), so an item only sleeps once it is already out of the streamed
region.

**Save cadence.** Pickups and drops save immediately (rare, cheap blobs); a
periodic autosave fires every 2 minutes; and `Engine::OnShutdown` saves on a
clean quit. Because pickup/drop persist on the spot, even a hard kill keeps the
latest change.

## Testing / verification

- `RX_ITEM_PROBE=1` (with `RX_PLAYER=1`) runs a one-shot debug harness a few
  seconds after spawn: it picks up the nearest loose item and drops it, logging
  each step — an input-free end-to-end exercise of catalog build → pickup → ref
  removal → drop → persist. Verified on real Skyrim data
  (`--data-dir ~/Music/SK --interior WhiterunBanneredMare`) under vkrun/NVIDIA:
  a picked-up book is added, the ref removed, dropped back, saved; on relaunch the
  log shows `loaded 1 defs, 1 removed refs, 1 live world item` — the drop persists
  and the original reference stays gone.
- The underlying save/load/hibernate primitives are covered by the engine's
  `inventory_test` (rx module).

## Gaps for later waves

- **Equip visuals / first-person equipment** — wave 3. `rx::inventory`'s
  `Equipment` slots exist but nothing equips or renders held items yet.
- **ARMO ground models** — armor has no static world mesh, so a dropped ARMO uses
  an invisible fallback box. Needs the biped ground-model path.
- **Authored NIF `bhk` collision** for dropped items (currently box-from-bounds).
- **Re-picking your own dropped items** — `rx::PickUpItem` is ready, but dropped
  `WorldItem` entities carry no `FormLink`, so the activation candidate gather
  does not see them yet.
- **MP replication** of ref removal to other clients (single-player + host
  authoritative today).
- **A lingering static collider** for a picked-up ref survives until its cell next
  unloads (the render entity is gone immediately; the streamer owns the body by
  cell, not by ref).
