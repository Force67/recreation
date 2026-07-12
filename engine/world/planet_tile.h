#ifndef RECREATION_WORLD_PLANET_TILE_H_
#define RECREATION_WORLD_PLANET_TILE_H_

#include <functional>
#include <string>

#include "asset/asset_database.h"
#include "bethesda/planet.h"
#include "core/math.h"
#include "ecs/world.h"
#include "physics/physics_world.h"

namespace rx::world {

// Generates a bounded procedural landing tile for a Starfield planet: a small
// block of exterior cells whose heightfield is synthesized with engine-native
// fractal noise (deterministic from the planet name + cell coords), whose ground
// material is picked from the decoded .biom biome map, plus scattered procedural
// boulders. This sidesteps Starfield's undocumented .btd terrain composition and
// realizes a walkable, biome-coloured surface through the same mesh/collider
// path the LAND streamer uses.
//
// This is an MVP first slice: terrain shape is synthetic (not Bethesda's real
// per-planet geometry) and scatter is procedural boulders (not PKIN POIs / GRAS
// flora). The biome -> ground-material choice is real (resolved from the .biom
// and the BIOM/LTEX/.mat chain when the CDB resolves it).
class PlanetTile {
 public:
  struct Uploads {
    std::function<bool(const asset::Mesh&)> mesh;
    std::function<bool(const asset::Texture&)> texture;
    std::function<bool(const asset::Material&)> material;
  };

  struct Config {
    i32 radius = 2;         // cells each side of the origin (radius=2 -> 5x5 tile)
    f32 cell_size = 4096.0f;  // Bethesda units per cell edge
    f32 units_to_meters = 0.01428f;
    f32 height_scale = 320.0f;  // peak-to-valley relief in Bethesda units (~4.6 m)
    f32 rock_density = 1.0f;    // multiplies the scatter count
  };

  PlanetTile(asset::AssetDatabase& assets, const bethesda::PlanetSurface& surface,
             const Config& config)
      : assets_(assets), surface_(surface), config_(config) {}

  void SetUploads(Uploads uploads) { uploads_ = std::move(uploads); }
  void set_physics(physics::PhysicsWorld* physics) { physics_ = physics; }

  // Realizes the whole tile into `world`: terrain meshes + colliders + scatter.
  // Returns the number of terrain cells spawned.
  u32 Generate(ecs::World& world);

  // A good camera spawn point (engine space) above the centre of the tile.
  Vec3 CameraSpawn() const;
  f32 GroundHeightAt(f32 engine_x, f32 engine_z) const;  // engine y at x/z

 private:
  // The material an area gets, built once per biome and cached.
  asset::AssetId GroundMaterial(u32 biome_index);
  // Synthesizes the 33x33 height grid (Bethesda units) for a cell.
  void CellHeights(i32 cell_x, i32 cell_y, f32* out) const;
  // Terrain height (Bethesda units) at a Bethesda-space x/y (the single source
  // of the heightfield, shared by the mesh, colliders, and scatter).
  f32 HeightBethesda(f32 bx, f32 by) const;
  // Fractal value noise (FBM) at a scaled coordinate, 0..1.
  f32 Noise(f32 x, f32 y) const;
  // Which biome index (into surface_.map.biome_ids) covers this cell.
  u32 BiomeIndexForCell(i32 cell_x, i32 cell_y) const;
  void SpawnScatter(ecs::World& world, i32 cell_x, i32 cell_y, u32 biome_index);
  const asset::Mesh* BoulderMesh(u64 seed, f32 tint[3]);

  Vec3 ToWorld(f32 bethesda_x, f32 bethesda_y, f32 bethesda_z) const;

  asset::AssetDatabase& assets_;
  const bethesda::PlanetSurface& surface_;
  Config config_;
  Uploads uploads_;
  physics::PhysicsWorld* physics_ = nullptr;
  u64 seed_ = 0;  // planet-name hash, base of every per-cell seed
  base::UnorderedMap<u32, asset::AssetId> biome_materials_;  // biome index -> material
  u32 spawned_scatter_ = 0;
};

}  // namespace rx::world

#endif  // RECREATION_WORLD_PLANET_TILE_H_
