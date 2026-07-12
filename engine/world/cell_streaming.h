#ifndef RECREATION_WORLD_CELL_STREAMING_H_
#define RECREATION_WORLD_CELL_STREAMING_H_

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include <functional>
#include <string>
#include <string_view>

#include "asset/asset_database.h"
#include "bethesda/game_profile.h"
#include "bethesda/load_order.h"
#include "core/math.h"
#include "ecs/world.h"
#include "physics/physics_world.h"
#include "render/pipeline/mesh_pipeline.h"
#include "world/grass_baker.h"
#include "world/land_baker.h"
#include "world/quest_world.h"

namespace rx::world {

// Effective authored lighting of an interior cell, resolved from its CELL XCLL
// subrecord and the referenced LGTM lighting template (LTMP), applying the
// per-group inherit flags (a set bit takes the template's value, a clear bit the
// cell's own). Colours are 0..1 (byte/255); fog distances are meters. Consumed by
// the frame loop, which pushes it onto the renderer's interior lighting settings.
struct InteriorLighting {
  bool valid = false;
  Vec3 ambient{0.05f, 0.05f, 0.06f};
  Vec3 directional_color{0.0f, 0.0f, 0.0f};
  Vec3 directional_dir{0.2f, -0.9f, 0.3f};  // engine-space travel direction
  f32 directional_intensity = 0.0f;
  Vec3 fog_near_color{0.0f, 0.0f, 0.0f};
  Vec3 fog_far_color{0.0f, 0.0f, 0.0f};
  f32 fog_near = 0.0f;  // meters
  f32 fog_far = 0.0f;   // meters; <= near disables fog
  f32 fog_power = 1.0f;
  f32 fog_max = 1.0f;
};

// Streams exterior cells of one worldspace around the camera. CELL/REFR/LAND
// records become entities with Transform + Renderable; meshes and terrain
// convert lazily under a per-tick budget so the frame loop never stalls on a
// cell boundary.
//
// This is the single place where Bethesda coordinates become engine
// coordinates: meshes stay in Bethesda object space (Z-up, 1 unit =
// 1.428 cm) and every spawned entity carries the conversion in its
// transform: engine = (x, z, -y) * 0.01428, i.e. a -90 degree rotation
// about X plus a uniform scale.
class CellStreamer {
 public:
  // Optional rigid body world: loaded cells then register terrain
  // colliders and water answers buoyancy queries.
  void set_physics(physics::PhysicsWorld* physics) { physics_ = physics; }

  // Optional quest world: loaded NPC references register their form->entity
  // mapping here so quests can target them and clients can apply replicated
  // actor transforms by form id.
  void set_quest_world(QuestWorld* quest_world) { quest_world_ = quest_world; }

  // Water surface height and flow at an engine-space position, for
  // buoyancy. Flow derives from the height gradient of neighboring cells.
  bool WaterHeightAt(const Vec3& position, f32* height, Vec3* flow);

  struct Uploads {
    std::function<bool(const asset::Mesh&)> mesh;
    std::function<bool(const asset::Texture&)> texture;
    std::function<bool(const asset::Material&)> material;
  };

  struct Settings {
    i32 load_radius = 3;       // cells around the camera cell
    u32 mesh_budget = 6;       // new mesh conversions/uploads per update
    u32 ref_budget = 192;      // reference instantiations per update
    f32 grass_density = 1.0f;  // multiplies every GRAS density, 0 disables
    bool distant_lod = false;  // load Bethesda .btr/.bto distant LOD for the horizon
    u32 distant_budget = 2;    // distant LOD quads loaded per update
    bool terrain_splat = true; // splat real land textures (off -> per-cell albedo bake)
  };

  CellStreamer(const bethesda::RecordStore& records, const bethesda::GameProfile& profile,
               asset::AssetDatabase& assets)
      : records_(records),
        assets_(assets),
        baker_(records, assets),
        grass_baker_(records, assets),
        cell_size_(profile.cell_size),
        units_to_meters_(profile.units_to_meters) {}

  void Configure(const Settings& settings) {
    settings_ = settings;
    // Reconfiguring (e.g. the trailer widening the primary's radius + distant LOD)
    // invalidates the caught-up state: a smaller-radius idle must not read as ready
    // under the new params, and the distant catalog must rebuild for the new radius.
    announced_idle_ = false;
    distant_discovered_ = false;
  }
  void SetUploads(Uploads uploads) { uploads_ = std::move(uploads); }

  // Water height for cells with the has-water flag when neither the cell
  // (XCLW) nor the worldspace (WRLD DNAM) provides one. Oblivion WRLDs carry
  // no DNAM at all; its exterior sea level is 0.
  void set_fallback_water_height(f32 height) { fallback_water_height_ = height; }

  // Translates all spawned content (and the camera anchor) by a fixed engine
  // space vector. Zero for the primary game; a secondary content domain
  // (a Fallout 4 worldspace streamed next to Skyrim) is offset so the two
  // worlds sit side by side in the shared scene instead of overlapping.
  void set_world_offset(const Vec3& offset) { world_offset_ = offset; }
  const Vec3& world_offset() const { return world_offset_; }

  // Streams cells around a fixed engine-space point instead of the live camera,
  // so a secondary worldspace shows one chosen region (set in that domain's own
  // pre-offset engine coordinates) held in place as a diorama.
  void set_fixed_anchor(const Vec3& anchor) {
    fixed_anchor_ = anchor;
    has_fixed_anchor_ = true;
  }
  const Vec3& fixed_anchor() const { return fixed_anchor_; }

  // Namespaces this streamer's mesh ids in the shared renderer. Asset paths
  // collide across games (Skyrim and Fallout 4 both ship "meshes/..."), so two
  // domains streaming at once would otherwise hash to the same renderer mesh and
  // BLAS key, overwriting each other's GPU buffers and corrupting ray tracing.
  // The matching upload callback must salt the same way (see engine wiring).
  // Zero (the default) leaves the primary game's ids untouched.
  void set_mesh_id_salt(u64 salt) { mesh_id_salt_ = salt; }

  // Notified on a load-door cell transition with the destination interior cell
  // id (0 when going outside) and whether it is interior. The runtime forwards
  // it to the managed world as a LocationChanged event.
  void set_on_location_change(std::function<void(u64, bool)> cb) {
    on_location_change_ = std::move(cb);
  }

  // Picks the worldspace to stream, e.g. "Tamriel". False if missing.
  bool SelectWorldspace(std::string_view editor_id);

  // Called each sim tick with the camera position in engine units.
  void Update(ecs::World& world, const Vec3& camera_position);

  // Loads one interior cell completely, no streaming. `camera_position`
  // receives a spawn point above the centroid of the placed references.
  bool LoadInterior(ecs::World& world, bethesda::GlobalFormId cell_id, Vec3* camera_position);

  // Runtime cell transitions for load doors. EnterInterior unloads everything
  // currently streamed, suspends exterior streaming, and loads `cell_id`
  // completely (its spawn-point fallback goes to `camera_position`).
  // EnterExterior unloads any active interior and lets Update resume streaming
  // the worldspace around the camera on the next tick. `in_interior` reports
  // whether streaming is currently suspended for an interior.
  bool EnterInterior(ecs::World& world, bethesda::GlobalFormId cell_id, Vec3* camera_position);
  void EnterExterior(ecs::World& world);
  bool in_interior() const { return interior_active_; }

  // The authored lighting of the active interior (XCLL/LGTM resolved), valid only
  // while in_interior(). The frame loop feeds it to the renderer each frame.
  const InteriorLighting& interior_lighting() const { return interior_lighting_; }

  // Drops everything this streamer has spawned (all exterior cells, any active
  // interior, and the distant LOD proxies). The trailer uses it to unload the
  // previous game before cutting to the next; a later Update re-streams from the
  // anchor.
  void UnloadAllCells(ecs::World& world);

  // Editor placement: converts a base form's world model, uploads it to the
  // shared renderer (salted for this domain), and spawns a standalone
  // renderable entity at an engine-space transform. Unlike a streamed ref the
  // entity belongs to no cell, so it persists until the caller destroys it;
  // `scale` is a user multiplier (1.0 = the model's native size). Returns
  // kInvalidEntity when the base resolves to no usable model. `out_mesh`, when
  // given, receives the renderer mesh id used. The runtime map editor drops
  // assets onto the world through this.
  ecs::Entity PlaceObject(ecs::World& world, bethesda::GlobalFormId base_id, const Vec3& position,
                          const f32 rotation[4], f32 scale, asset::AssetId* out_mesh = nullptr);

  // Appends the point lights from every loaded cell's placed LIGH refs (torches,
  // sconces, lamps) to `out`, so dungeons and night scenes get local lighting.
  // Kept nearest-to-camera when the streamed count would overflow the renderer's
  // frame light budget. Matches MapEditor::CollectLights' shape; the frame loop
  // calls both and the renderer clusters the union.
  void CollectLights(base::Vector<render::PointLight>& out) const;

  // Appends the projected decals from every loaded cell's placed TXST refs
  // (blood pools, burn marks, shadowmarks, giant paint) to `out`, nearest to
  // the camera first when over the renderer's frame decal budget. The frame
  // loop feeds them into FrameView::decals next to the CollectLights call.
  void CollectDecals(base::Vector<render::Decal>& out) const;

  // The shared decal atlas the collected decals' uv rects index, built once
  // from every decal-capable TXST when the first placed decal streams in. Ids
  // are salted for this domain like the mesh ids; version stays 0 until the
  // atlas exists, so the frame loop knows when to point the renderer at it.
  asset::AssetId decal_atlas_id() const { return {decal_atlas_.id.hash ^ mesh_id_salt_}; }
  asset::AssetId decal_atlas_normal_id() const {
    return {decal_atlas_normal_.id.hash ^ mesh_id_salt_};
  }
  u32 decal_atlas_version() const { return decal_atlas_version_; }

  // Terrain height (engine units) at an engine space x/z from LAND data.
  bool GroundHeight(f32 engine_x, f32 engine_z, f32* engine_y) const;

  // World-space rect (min_x, min_z, max_x, max_z) of the contiguous cell ring
  // around the camera whose full-detail terrain is spawned; all zeros while
  // none is. Distant terrain-LOD draws sink their vertices inside it (see
  // render::FrameView::detail_rect).
  const f32* detail_rect() const { return detail_rect_; }

  // True once streaming has caught up: every in-range cell is loaded and its
  // incremental conversion is done, no budget was left pending, and (when on) the
  // distant LOD is fully drained. The trailer waits on this before revealing a
  // freshly cut-to game. Resets on UnloadAllCells / interior transitions.
  bool caught_up() const { return announced_idle_; }

  size_t loaded_cell_count() const { return loaded_.size(); }
  size_t spawned_entity_count() const { return spawned_entities_; }
  size_t spawned_npc_count() const { return spawned_npcs_; }
  size_t converted_mesh_count() const { return base_meshes_.size(); }

 private:
  struct LoadedCell {
    base::Vector<ecs::Entity> entities;
    // Point lights from this cell's placed LIGH refs, baked at spawn (statics
    // never move) so they drop out when the cell unloads.
    base::Vector<render::PointLight> lights;
    // Projected decals from this cell's placed TXST refs, baked at spawn like
    // the lights. The world position rides along for the nearest-to-camera cut
    // (the packed Decal only carries the inverse box rows).
    struct PlacedDecal {
      render::Decal decal;
      Vec3 position;
    };
    base::Vector<PlacedDecal> decals;
    const bethesda::RecordStore::ExteriorCell* source = nullptr;
    physics::BodyId terrain_body = 0;
    base::Vector<physics::BodyId> bodies;  // static ref colliders
    u32 next_ref = 0;
    bool terrain_done = false;
    bool grass_done = false;
    bool done = false;
  };

  // Returns false when the budget ran out before the cell completed.
  bool LoadCellIncremental(ecs::World& world, i16 grid_x, i16 grid_y, LoadedCell& cell,
                           u32& mesh_budget, u32& ref_budget);
  void UnloadCell(ecs::World& world, u32 key);
  // Destroys the active interior's entities and colliders (see interior_cell_).
  void UnloadInterior(ecs::World& world);
  // Resolves the active interior's authored lighting into interior_lighting_ from
  // the cell's XCLL and its LGTM template per the inherit flags.
  void ResolveInteriorLighting(bethesda::GlobalFormId cell_id);
  bool SpawnTerrain(ecs::World& world, i16 grid_x, i16 grid_y, LoadedCell& cell);
  // Distant LOD: discovers the coarsest .btr (terrain) + .bto (object) quads of
  // the streamed worldspace once, then drains them under a budget. They cover the
  // whole map cheaply (a few dozen quads) so the mesh-shader cull does the rest.
  void DiscoverDistantQuads();
  bool SpawnDistantQuad(ecs::World& world, size_t index);
  // Starfield hand-built worldspaces carry no LAND: their natural ground ships
  // as per-cell instance NIFs (meshes/terrain/<worldspace>/objects/), each a
  // list of STAT form placements. Spawns every instance like a placed ref.
  bool SpawnInstancedTerrain(ecs::World& world, i16 grid_x, i16 grid_y, LoadedCell& cell);
  // Instantiates a pack-in (Starfield PKIN prefab): the refs of its template
  // cell spawn composed onto the given Bethesda-space transform (position,
  // rotation quaternion, uniform scale). Recurses into nested pack-ins.
  bool SpawnPackIn(ecs::World& world, i16 grid_x, i16 grid_y, bethesda::GlobalFormId pkin_id,
                   const f32 position[3], const f32 rotation[4], f32 scale, LoadedCell& cell,
                   bool interior, int depth);
  bool SpawnWater(ecs::World& world, i16 grid_x, i16 grid_y, LoadedCell& cell);
  bool SpawnGrass(ecs::World& world, i16 grid_x, i16 grid_y, LoadedCell& cell);
  // Water level of the cell in record units; false when the cell has none.
  bool CellWaterHeight(i16 grid_x, i16 grid_y, const LoadedCell& cell, f32* height) const;
  void AddTerrainCollider(i16 grid_x, i16 grid_y, LoadedCell& cell, const f32* heights);
  // Floor estimate (engine y) for a cell with no LAND, from its placed refs.
  bool RefsGroundHeight(u32 grid_key, const bethesda::RecordStore::ExteriorCell& cell,
                        f32* engine_y) const;
  bool SpawnReference(ecs::World& world, i16 grid_x, i16 grid_y, u64 ref_id, LoadedCell& cell,
                      u32& mesh_budget, bool interior);
  // When `base_id` is a LIGH, parses its DATA (radius/colour) + FNAM fade and any
  // REFR XRDS radius override into a point light at `position` and records it on
  // the cell. No-op for other base types or when RX_PLACED_LIGHTS is off.
  void AddPlacedLight(bethesda::GlobalFormId base_id, const bethesda::Record& refr,
                      const Vec3& position, LoadedCell& cell);
  // When `base_id` is a TXST, builds a projected decal box from the REFR
  // placement (rotation/XSCL) and the TXST's DODT extents/tint, uv'd into the
  // shared decal atlas, and records it on the cell. Builds the atlas on first
  // use. No-op for other base types or when RX_PLACED_DECALS is off.
  void AddPlacedDecal(bethesda::GlobalFormId base_id, bethesda::GlobalFormId ref_id,
                      const bethesda::Record& refr, const Vec3& position, LoadedCell& cell);
  // Builds the decal atlas once: every TXST with decal data (DODT) gets its
  // TX00/TX01 decoded into 256px tiles (subtexture sheets split into one tile
  // per variant, textures shared across TXSTs deduped), then uploads both
  // atlas pages and records per-base extents/uv info in decal_bases_.
  void EnsureDecalAtlas();
  const asset::Mesh* MeshForBase(bethesda::GlobalFormId base_id, u32& mesh_budget,
                                 bool& budget_exceeded);
  bool EnsureUploaded(const asset::Mesh& mesh);
  void EnsureLandMaterial();
  // Water plane tinted by the cell's WATR type (XCWT, else the worldspace NAM2
  // default). ResolveCellWaterForm picks the form; WaterMeshForCell caches a
  // shallow-colour-tinted quad per WATR form; EnsureWaterMesh builds it.
  bethesda::GlobalFormId ResolveCellWaterForm(const LoadedCell& cell) const;
  const asset::Mesh* WaterMeshForCell(const LoadedCell& cell);
  const asset::Mesh* EnsureWaterMesh(u64 form_key, const f32 tint[3]);
  // Bethesda game-space (x, y, z) to engine space, including world_offset_.
  Vec3 ToWorld(f32 bethesda_x, f32 bethesda_y, f32 bethesda_z) const;
  // The renderer-side mesh key for a converted asset, salted for this domain.
  asset::AssetId RenderMeshId(asset::AssetId id) const { return {id.hash ^ mesh_id_salt_}; }

  const bethesda::RecordStore& records_;
  asset::AssetDatabase& assets_;
  LandBaker baker_;
  GrassBaker grass_baker_;
  Settings settings_;
  Uploads uploads_;
  physics::PhysicsWorld* physics_ = nullptr;
  QuestWorld* quest_world_ = nullptr;
  Vec3 world_offset_{0.0f, 0.0f, 0.0f};  // engine-space shift of all spawned content
  Vec3 fixed_anchor_{0.0f, 0.0f, 0.0f};  // streaming center when has_fixed_anchor_
  bool has_fixed_anchor_ = false;
  u64 mesh_id_salt_ = 0;  // namespaces mesh ids in the shared renderer (per domain)
  bethesda::GlobalFormId worldspace_;
  std::string worldspace_edid_;  // lowercased, for building distant LOD paths
  const bethesda::RecordStore::ExteriorGrid* grid_ = nullptr;
  base::UnorderedMap<u32, LoadedCell> loaded_;

  // Distant LOD quads (the coarsest .btr/.bto of the worldspace). Discovered once
  // on first Update, then drained: each becomes a persistent renderable proxy.
  struct DistantQuad {
    std::string path;
    i32 cell_x = 0;  // SW cell of the quad (only used by terrain, which is quad-local)
    i32 cell_y = 0;
    bool object = false;  // .bto: vertices are absolute world; .btr: quad-local
  };
  base::Vector<DistantQuad> distant_quads_;
  size_t distant_next_ = 0;          // next quad to load from distant_quads_
  bool distant_discovered_ = false;  // catalog built for the current worldspace
  base::Vector<ecs::Entity> distant_entities_;
  // While in an interior, exterior streaming is suspended and the interior's
  // placed refs are tracked here so a later transition can unload them.
  bool interior_active_ = false;
  LoadedCell interior_cell_;
  InteriorLighting interior_lighting_;
  std::function<void(u64, bool)> on_location_change_;  // load-door transition hook
  // Base form id -> converted mesh (null when the base has no usable model),
  // so failures are only diagnosed once.
  base::UnorderedMap<u64, const asset::Mesh*> base_meshes_;
  // Decoded LAND heights (game units) per cell GridKey, so a GroundHeight sweep
  // (the editor's per-frame placement raycast) reuses one decode per cell
  // instead of re-parsing the LAND record on every sample. Cleared when the
  // streamed worldspace changes; LAND data is immutable so entries never go stale.
  mutable base::UnorderedMap<u32, base::Vector<f32>> ground_cache_;
  // Estimated floor height (engine y) for cells with no LAND record, the city
  // worldspaces (New Atlantis) where the ground is the building meshes, not a
  // heightfield. Derived once from the cell's placed refs and cached like the
  // LAND heights above. NaN marks a cell with too few refs to estimate.
  mutable base::UnorderedMap<u32, f32> refs_ground_cache_;
  base::UnorderedMap<u64, bool> uploaded_;  // mesh/texture/material id set
  asset::AssetId land_material_;
  // Per-game world units (GameProfile): record units per cell edge and record
  // position units -> engine metres. Mesh-space scaling stays the fixed game
  // unit -> metre constant (converted meshes are always in game-unit space).
  f32 cell_size_ = 4096.0f;
  f32 units_to_meters_ = 0.01428f;
  f32 default_water_height_ = -3.0e38f;  // worldspace WRLD DNAM, record units
  f32 fallback_water_height_ = -3.0e38f;  // when the WRLD has no DNAM (Oblivion)
  bethesda::GlobalFormId default_water_form_;  // worldspace WRLD NAM2 (WATR), else invalid
  // Starfield worldspaces carry a WRLD-level water table (XCLW cell pairs +
  // WHGT heights): listed cells hold water at their own height instead of the
  // worldspace default (New Atlantis' upper lake vs the spaceport lake).
  base::UnorderedMap<u32, f32> water_table_;  // GridKey -> height, record units
  bool has_water_table_ = false;
  // WATR form (packed) -> tinted water quad mesh id; key 0 is the untinted
  // fallback plane. Cached so each water type is parsed and built once.
  base::UnorderedMap<u64, asset::AssetId> water_meshes_;
  size_t spawned_entities_ = 0;
  size_t spawned_npcs_ = 0;
  size_t terrain_instances_ = 0;  // Starfield instanced-terrain placements
  size_t water_planes_ = 0;
  u32 skipped_refs_ = 0;
  bool announced_idle_ = false;
  f32 detail_rect_[4] = {0, 0, 0, 0};  // see detail_rect()
  Vec3 last_camera_{0.0f, 0.0f, 0.0f};  // last Update anchor, for nearest-light culling
  mutable size_t logged_light_count_ = ~size_t{0};  // last CollectLights count logged

  // Placed-decal state (see EnsureDecalAtlas). A base's entry carries the
  // projection box half extents (meters, before XSCL), DODT tint, and its
  // tile run in the atlas; invalid bases (no DODT/texture) are absent.
  struct DecalBase {
    f32 half_w = 0, half_h = 0, half_d = 0;
    f32 tint[3] = {1, 1, 1};
    f32 normal_strength = 0;  // 1 when the TX01 normal page decoded
    u32 tile = 0;             // first atlas tile
    u32 tiles = 1;            // subtexture variants, consecutive tiles
  };
  base::UnorderedMap<u64, DecalBase> decal_bases_;
  asset::Texture decal_atlas_;         // rgba8 albedo page, uploaded once
  asset::Texture decal_atlas_normal_;  // matching normal page
  bool decal_atlas_built_ = false;
  u32 decal_atlas_version_ = 0;
  mutable size_t logged_decal_count_ = ~size_t{0};  // last CollectDecals count logged
};

}  // namespace rx::world

#endif  // RECREATION_WORLD_CELL_STREAMING_H_
