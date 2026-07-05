#ifndef RECREATION_WORLD_CELL_STREAMING_H_
#define RECREATION_WORLD_CELL_STREAMING_H_

#include <functional>
#include <string_view>

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "asset/asset_database.h"
#include "bethesda/load_order.h"
#include "core/math.h"
#include "ecs/world.h"
#include "physics/physics_world.h"
#include "world/grass_baker.h"
#include "world/land_baker.h"
#include "world/quest_world.h"

namespace rec::world {

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
    i32 load_radius = 3;          // cells around the camera cell
    u32 mesh_budget = 6;          // new mesh conversions/uploads per update
    u32 ref_budget = 192;         // reference instantiations per update
    f32 grass_density = 1.0f;     // multiplies every GRAS density, 0 disables
  };

  CellStreamer(const bethesda::RecordStore& records, asset::AssetDatabase& assets)
      : records_(records),
        assets_(assets),
        baker_(records, assets),
        grass_baker_(records, assets) {}

  void Configure(const Settings& settings) { settings_ = settings; }
  void SetUploads(Uploads uploads) { uploads_ = std::move(uploads); }

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

  // Terrain height (engine units) at an engine space x/z from LAND data.
  bool GroundHeight(f32 engine_x, f32 engine_z, f32* engine_y) const;

  size_t loaded_cell_count() const { return loaded_.size(); }
  size_t spawned_entity_count() const { return spawned_entities_; }
  size_t spawned_npc_count() const { return spawned_npcs_; }
  size_t converted_mesh_count() const { return base_meshes_.size(); }

 private:
  struct LoadedCell {
    base::Vector<ecs::Entity> entities;
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
  bool SpawnTerrain(ecs::World& world, i16 grid_x, i16 grid_y, LoadedCell& cell);
  bool SpawnWater(ecs::World& world, i16 grid_x, i16 grid_y, LoadedCell& cell);
  bool SpawnGrass(ecs::World& world, i16 grid_x, i16 grid_y, LoadedCell& cell);
  // Water level of the cell in game units; false when the cell has none.
  bool CellWaterHeight(const LoadedCell& cell, f32* height) const;
  void AddTerrainCollider(i16 grid_x, i16 grid_y, LoadedCell& cell, const f32* heights);
  bool SpawnReference(ecs::World& world, i16 grid_x, i16 grid_y, u64 ref_id, LoadedCell& cell,
                      u32& mesh_budget, bool interior);
  const asset::Mesh* MeshForBase(bethesda::GlobalFormId base_id, u32& mesh_budget,
                                 bool& budget_exceeded);
  bool EnsureUploaded(const asset::Mesh& mesh);
  void EnsureLandMaterial();
  const asset::Mesh* EnsureWaterMesh();

  const bethesda::RecordStore& records_;
  asset::AssetDatabase& assets_;
  LandBaker baker_;
  GrassBaker grass_baker_;
  Settings settings_;
  Uploads uploads_;
  physics::PhysicsWorld* physics_ = nullptr;
  QuestWorld* quest_world_ = nullptr;
  bethesda::GlobalFormId worldspace_;
  const bethesda::RecordStore::ExteriorGrid* grid_ = nullptr;
  base::UnorderedMap<u32, LoadedCell> loaded_;
  // While in an interior, exterior streaming is suspended and the interior's
  // placed refs are tracked here so a later transition can unload them.
  bool interior_active_ = false;
  LoadedCell interior_cell_;
  std::function<void(u64, bool)> on_location_change_;  // load-door transition hook
  // Base form id -> converted mesh (null when the base has no usable model),
  // so failures are only diagnosed once.
  base::UnorderedMap<u64, const asset::Mesh*> base_meshes_;
  base::UnorderedMap<u64, bool> uploaded_;  // mesh/texture/material id set
  asset::AssetId land_material_;
  f32 default_water_height_ = -3.0e38f;  // worldspace WRLD DNAM, game units
  size_t spawned_entities_ = 0;
  size_t spawned_npcs_ = 0;
  size_t water_planes_ = 0;
  u32 skipped_refs_ = 0;
  bool announced_idle_ = false;
};

}  // namespace rec::world

#endif  // RECREATION_WORLD_CELL_STREAMING_H_
