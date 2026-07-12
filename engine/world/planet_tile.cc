#include "world/planet_tile.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <base/option.h>

#include "core/log.h"
#include "world/components.h"

namespace rx::world {
namespace {

// RX_PLANET_TEXTURE=0 drops the resolved ground texture and shades the tile on
// the flat biome tint, to isolate terrain shape from texture/UV artifacts.
static base::Option<bool> GroundTexture{"planet.texture", true, "RX_PLANET_TEXTURE"};

constexpr u32 kGrid = 33;  // heightfield samples per cell edge, like LAND
constexpr f32 kAxisChange[4] = {-0.70710678f, 0.0f, 0.0f, 0.70710678f};  // -90deg about X

// Same xorshift PRNG the grass baker uses, for a per-tile deterministic seed.
struct Rng {
  explicit Rng(u64 seed) : state(seed * 0x9e3779b97f4a7c15ull | 1) {}
  u64 state;
  u64 Next() {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545f4914f6cdd1dull;
  }
  f32 Uniform() { return static_cast<f32>(Next() >> 40) * (1.0f / 16777216.0f); }
  f32 Range(f32 a, f32 b) { return a + (b - a) * Uniform(); }
};

u64 HashString(const std::string& s) {
  u64 h = 1469598103934665603ull;  // FNV-1a
  for (char c : s) {
    h ^= static_cast<u8>(c);
    h *= 1099511628211ull;
  }
  return h;
}

// Hash-based value noise: a smooth deterministic field from a per-lattice-point
// pseudo-random value. Good enough to read as procedural terrain relief.
f32 Hash2(i32 x, i32 y, u64 seed) {
  u64 h = static_cast<u64>(static_cast<u32>(x)) * 0x9e3779b97f4a7c15ull;
  h ^= static_cast<u64>(static_cast<u32>(y)) * 0xc2b2ae3d27d4eb4full;
  h ^= seed + 0x165667b19e3779f9ull;
  h ^= h >> 29;
  h *= 0xbf58476d1ce4e5b9ull;
  h ^= h >> 32;
  return static_cast<f32>(h >> 40) * (1.0f / 16777216.0f);  // 0..1
}

f32 Smooth(f32 t) { return t * t * (3.0f - 2.0f * t); }

f32 ValueNoise(f32 x, f32 y, u64 seed) {
  const i32 xi = static_cast<i32>(std::floor(x));
  const i32 yi = static_cast<i32>(std::floor(y));
  const f32 fx = Smooth(x - static_cast<f32>(xi));
  const f32 fy = Smooth(y - static_cast<f32>(yi));
  const f32 a = Hash2(xi, yi, seed);
  const f32 b = Hash2(xi + 1, yi, seed);
  const f32 c = Hash2(xi, yi + 1, seed);
  const f32 d = Hash2(xi + 1, yi + 1, seed);
  return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}

}  // namespace

Vec3 PlanetTile::ToWorld(f32 bethesda_x, f32 bethesda_y, f32 bethesda_z) const {
  return {bethesda_x * config_.units_to_meters, bethesda_z * config_.units_to_meters,
          -bethesda_y * config_.units_to_meters};
}

f32 PlanetTile::Noise(f32 x, f32 y) const {
  // Fractional Brownian motion over the value noise, four octaves. x/y are
  // already scaled by the caller's base frequency. Four octaves keeps the finest
  // lattice (~2x the vertex spacing) above the Nyquist limit, so no spikes.
  f32 sum = 0, amp = 0.5f, freq = 1.0f, norm = 0;
  for (int o = 0; o < 4; ++o) {
    sum += amp * ValueNoise(x * freq, y * freq, seed_ + static_cast<u64>(o) * 7919ull);
    norm += amp;
    amp *= 0.5f;
    freq *= 2.03f;
  }
  return sum / norm;  // 0..1
}

u32 PlanetTile::BiomeIndexForCell(i32 cell_x, i32 cell_y) const {
  if (surface_.map.biome_ids.empty()) return 0;
  // Map the small tile of cells onto a patch of the biome grid so a
  // multi-biome planet shows more than one ground. Centre the tile on the
  // middle of hemisphere 0.
  const u32 dim = bethesda::BiomeMap::kDim;
  const i32 gx = static_cast<i32>(dim / 2) + cell_x * 6;
  const i32 gy = static_cast<i32>(dim / 2) + cell_y * 6;
  const u32 raw = surface_.map.BiomeAt(0, static_cast<u32>(std::clamp(gx, 0, static_cast<i32>(dim) - 1)),
                                       static_cast<u32>(std::clamp(gy, 0, static_cast<i32>(dim) - 1)));
  for (u32 i = 0; i < surface_.map.biome_ids.size(); ++i)
    if (surface_.map.biome_ids[i] == raw) return i;
  return 0;
}

asset::AssetId PlanetTile::GroundMaterial(u32 biome_index) {
  if (const asset::AssetId* cached = biome_materials_.find(biome_index)) return *cached;

  const bethesda::BiomeGround* ground =
      biome_index < surface_.grounds.size() ? &surface_.grounds[biome_index] : nullptr;

  asset::Material material;
  const std::string name = surface_.name + "/ground/" + std::to_string(biome_index);
  material.id = asset::MakeAssetId(name);
  material.roughness_factor = 1.0f;

  if (ground) {
    // The BMC map colour is the ground tint fallback (and multiplies the
    // texture when one resolves). Bias away from pure black so barren biomes
    // are not invisible.
    material.base_color_factor[0] = std::max(0.18f, ground->tint[0]);
    material.base_color_factor[1] = std::max(0.18f, ground->tint[1]);
    material.base_color_factor[2] = std::max(0.18f, ground->tint[2]);
    if (!ground->base_color.empty() && GroundTexture.get()) {
      if (const asset::Texture* t = assets_.LoadTexture(ground->base_color)) {
        material.base_color = t->id;
        // A resolved texture already carries the colour; keep the factor neutral
        // so we do not double-darken.
        material.base_color_factor[0] = material.base_color_factor[1] =
            material.base_color_factor[2] = 1.0f;
      }
    }
    if (!ground->normal.empty()) {
      if (const asset::Texture* t = assets_.LoadTexture(ground->normal)) material.normal = t->id;
    }
  }
  assets_.AddMaterial(material);
  biome_materials_.insert(biome_index, material.id);
  return material.id;
}

// Base noise wavelength ~0.9 cell, so a few broad hills sit across the tile. The
// finest of the 5 FBM octaves is ~2.03^4 higher frequency, wavelength ~220
// units, still ~2x the ~128-unit vertex spacing (no per-vertex spikes). Height
// in Bethesda units.
f32 PlanetTile::HeightBethesda(f32 bx, f32 by) const {
  const f32 freq = 1.0f / (config_.cell_size * 0.9f);
  f32 n = Noise(bx * freq, by * freq);
  n = n * n * (3.0f - 2.0f * n);  // smootherstep: flatter valleys, rounder peaks
  return (n - 0.5f) * config_.height_scale;
}

void PlanetTile::CellHeights(i32 cell_x, i32 cell_y, f32* out) const {
  const f32 spacing = config_.cell_size / (kGrid - 1);
  for (u32 r = 0; r < kGrid; ++r) {
    for (u32 c = 0; c < kGrid; ++c) {
      const f32 bx = static_cast<f32>(cell_x) * config_.cell_size + static_cast<f32>(c) * spacing;
      const f32 by = static_cast<f32>(cell_y) * config_.cell_size + static_cast<f32>(r) * spacing;
      out[r * kGrid + c] = HeightBethesda(bx, by);
    }
  }
}

f32 PlanetTile::GroundHeightAt(f32 engine_x, f32 engine_z) const {
  // engine (x,z) -> bethesda (x,y): bethesda_x = engine_x/u, bethesda_y = -engine_z/u
  const f32 bx = engine_x / config_.units_to_meters;
  const f32 by = -engine_z / config_.units_to_meters;
  return HeightBethesda(bx, by) * config_.units_to_meters;
}

const asset::Mesh* PlanetTile::BoulderMesh(u64 seed, f32 tint[3]) {
  const asset::AssetId id = asset::MakeAssetId(surface_.name + "/rock/" + std::to_string(seed));
  if (const asset::Mesh* existing = assets_.FindMesh(id)) return existing;

  // A jittered icosphere-ish blob: a low-poly boulder, deterministic per seed.
  Rng rng(seed);
  asset::Material material;
  material.id = asset::MakeAssetId(surface_.name + "/rockmat/" + std::to_string(seed % 8));
  material.roughness_factor = 0.95f;
  material.base_color_factor[0] = std::max(0.12f, tint[0] * 0.8f);
  material.base_color_factor[1] = std::max(0.12f, tint[1] * 0.8f);
  material.base_color_factor[2] = std::max(0.12f, tint[2] * 0.8f);
  assets_.AddMaterial(material);

  asset::Mesh mesh;
  mesh.id = id;
  mesh.lods.emplace_back();
  asset::MeshLod& lod = mesh.lods[0];

  // Build a UV-sphere and displace radii by noise for an irregular rock. Radius
  // in Bethesda units (~30-90 units = ~0.4-1.3 m).
  const u32 kRings = 6, kSegs = 8;
  const f32 radius = rng.Range(30.0f, 90.0f);
  f32 max_r = 0;
  for (u32 ring = 0; ring <= kRings; ++ring) {
    const f32 v = static_cast<f32>(ring) / kRings;
    const f32 theta = v * 3.14159265f;
    for (u32 seg = 0; seg <= kSegs; ++seg) {
      const f32 u = static_cast<f32>(seg) / kSegs;
      const f32 phi = u * 6.2831853f;
      const f32 jitter = 0.65f + 0.45f * rng.Uniform();
      const f32 r = radius * jitter;
      asset::Vertex vert{};
      vert.position[0] = r * std::sin(theta) * std::cos(phi);
      vert.position[1] = r * std::sin(theta) * std::sin(phi);
      vert.position[2] = r * std::cos(theta) * 0.7f;  // squash so it sits flat
      vert.normal[0] = std::sin(theta) * std::cos(phi);
      vert.normal[1] = std::sin(theta) * std::sin(phi);
      vert.normal[2] = std::cos(theta);
      vert.tangent[0] = 1;
      vert.tangent[3] = 1;
      vert.uv[0] = u;
      vert.uv[1] = v;
      max_r = std::max(max_r, r);
      lod.vertices.push_back(vert);
    }
  }
  const u32 stride = kSegs + 1;
  for (u32 ring = 0; ring < kRings; ++ring) {
    for (u32 seg = 0; seg < kSegs; ++seg) {
      const u32 a = ring * stride + seg;
      const u32 b = a + 1;
      const u32 c = a + stride;
      const u32 d = c + 1;
      lod.indices.push_back(a);
      lod.indices.push_back(c);
      lod.indices.push_back(b);
      lod.indices.push_back(b);
      lod.indices.push_back(c);
      lod.indices.push_back(d);
    }
  }
  asset::Submesh submesh;
  submesh.index_count = static_cast<u32>(lod.indices.size());
  submesh.material = material.id;
  lod.submeshes.push_back(submesh);
  mesh.bounds_radius = max_r;
  return assets_.AddMesh(std::move(mesh));
}

void PlanetTile::SpawnScatter(ecs::World& world, i32 cell_x, i32 cell_y, u32 biome_index) {
  if (config_.rock_density <= 0.0f) return;
  const bethesda::BiomeGround* ground =
      biome_index < surface_.grounds.size() ? &surface_.grounds[biome_index] : nullptr;
  f32 tint[3] = {0.5f, 0.5f, 0.5f};
  if (ground) std::memcpy(tint, ground->tint, sizeof(tint));

  Rng rng(seed_ ^ (static_cast<u64>(static_cast<u32>(cell_x)) << 20) ^
          (static_cast<u64>(static_cast<u32>(cell_y)) << 4) ^ 0x0c4b0c4bull);
  const u32 count = static_cast<u32>(12.0f * config_.rock_density);
  for (u32 i = 0; i < count; ++i) {
    const f32 lx = rng.Uniform() * config_.cell_size;
    const f32 ly = rng.Uniform() * config_.cell_size;
    const f32 bx = static_cast<f32>(cell_x) * config_.cell_size + lx;
    const f32 by = static_cast<f32>(cell_y) * config_.cell_size + ly;
    const f32 bz = HeightBethesda(bx, by);

    const asset::Mesh* rock = BoulderMesh(rng.Next(), tint);
    if (!rock) continue;
    if (uploads_.mesh) {
      const asset::Material* mat = assets_.FindMaterial(rock->lods[0].submeshes[0].material);
      if (mat) uploads_.material(*mat);
      uploads_.mesh(*rock);
    }
    ecs::Entity entity = world.Create();
    Transform transform;
    const Vec3 pos = ToWorld(bx, by, bz);
    transform.position[0] = pos.x;
    transform.position[1] = pos.y;
    transform.position[2] = pos.z;
    std::memcpy(transform.rotation, kAxisChange, sizeof(transform.rotation));
    transform.scale = config_.units_to_meters;
    world.Add(entity, transform);
    world.Add(entity, Renderable{rock->id});
    ++spawned_scatter_;
  }
}

u32 PlanetTile::Generate(ecs::World& world) {
  seed_ = HashString(surface_.name);
  u32 cells = 0;

  for (i32 cy = -config_.radius; cy <= config_.radius; ++cy) {
    for (i32 cx = -config_.radius; cx <= config_.radius; ++cx) {
      f32 heights[kGrid * kGrid];
      CellHeights(cx, cy, heights);
      const u32 biome_index = BiomeIndexForCell(cx, cy);
      const asset::AssetId material_id = GroundMaterial(biome_index);

      // Build the terrain mesh (same layout as LAND SpawnTerrain).
      asset::Mesh mesh;
      mesh.id = asset::MakeAssetId(surface_.name + "/tile/" + std::to_string(cx) + "_" +
                                   std::to_string(cy));
      mesh.lods.emplace_back();
      asset::MeshLod& lod = mesh.lods[0];
      const f32 spacing = config_.cell_size / (kGrid - 1);
      f32 min_h = 1e30f, max_h = -1e30f;
      for (u32 r = 0; r < kGrid; ++r) {
        for (u32 c = 0; c < kGrid; ++c) {
          const u32 idx = r * kGrid + c;
          asset::Vertex v{};
          v.position[0] = static_cast<f32>(c) * spacing;
          v.position[1] = static_cast<f32>(r) * spacing;
          v.position[2] = heights[idx];
          min_h = std::min(min_h, heights[idx]);
          max_h = std::max(max_h, heights[idx]);
          v.normal[2] = 1;
          v.tangent[0] = 1;
          v.tangent[3] = 1;
          v.uv[0] = static_cast<f32>(c) / (kGrid - 1) * 8.0f;  // tile the ground texture
          v.uv[1] = static_cast<f32>(r) / (kGrid - 1) * 8.0f;
          lod.vertices.push_back(v);
        }
      }
      // Compute vertex normals from the heightfield so lighting reads the relief.
      for (u32 r = 0; r < kGrid; ++r) {
        for (u32 c = 0; c < kGrid; ++c) {
          const u32 cl = c > 0 ? c - 1 : c, cr = c + 1 < kGrid ? c + 1 : c;
          const u32 rd = r > 0 ? r - 1 : r, ru = r + 1 < kGrid ? r + 1 : r;
          const f32 hl = heights[r * kGrid + cl], hr = heights[r * kGrid + cr];
          const f32 hd = heights[rd * kGrid + c], hu = heights[ru * kGrid + c];
          Vec3 n{(hl - hr), (hd - hu), 2.0f * spacing};
          const f32 len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
          asset::Vertex& v = lod.vertices[r * kGrid + c];
          if (len > 0) {
            v.normal[0] = n.x / len;
            v.normal[1] = n.y / len;
            v.normal[2] = n.z / len;
          }
        }
      }
      for (u32 r = 0; r + 1 < kGrid; ++r) {
        for (u32 c = 0; c + 1 < kGrid; ++c) {
          const u32 v0 = r * kGrid + c, v1 = v0 + 1, v2 = v0 + kGrid, v3 = v2 + 1;
          lod.indices.push_back(v0);
          lod.indices.push_back(v1);
          lod.indices.push_back(v2);
          lod.indices.push_back(v1);
          lod.indices.push_back(v3);
          lod.indices.push_back(v2);
        }
      }
      asset::Submesh submesh;
      submesh.index_count = static_cast<u32>(lod.indices.size());
      submesh.material = material_id;
      lod.submeshes.push_back(submesh);
      mesh.bounds_center[0] = config_.cell_size * 0.5f;
      mesh.bounds_center[1] = config_.cell_size * 0.5f;
      mesh.bounds_center[2] = (min_h + max_h) * 0.5f;
      mesh.bounds_radius = std::sqrt(2 * config_.cell_size * 0.5f * config_.cell_size * 0.5f +
                                     (max_h - min_h) * (max_h - min_h) * 0.25f);
      const asset::Mesh* built = assets_.AddMesh(std::move(mesh));

      // Upload textures -> material -> mesh (headless leaves uploads_ empty).
      if (uploads_.mesh) {
        const asset::Material* mat = assets_.FindMaterial(material_id);
        if (mat) {
          if (mat->base_color)
            if (const asset::Texture* t = assets_.FindTexture(mat->base_color)) uploads_.texture(*t);
          if (mat->normal)
            if (const asset::Texture* t = assets_.FindTexture(mat->normal)) uploads_.texture(*t);
          uploads_.material(*mat);
        }
        uploads_.mesh(*built);
      }

      // Terrain collider (engine-space heightfield, rows flipped like LAND).
      if (physics_) {
        f32 engine_heights[kGrid * kGrid];
        for (u32 j = 0; j < kGrid; ++j)
          for (u32 i = 0; i < kGrid; ++i) {
            const u32 row = kGrid - 1 - j;
            engine_heights[j * kGrid + i] = heights[row * kGrid + i] * config_.units_to_meters;
          }
        const Vec3 origin{static_cast<f32>(cx) * config_.cell_size * config_.units_to_meters, 0.0f,
                          -(static_cast<f32>(cy) + 1.0f) * config_.cell_size *
                              config_.units_to_meters};
        physics_->AddHeightField(origin, engine_heights, kGrid,
                                 config_.cell_size * config_.units_to_meters);
      }

      // Spawn the terrain entity.
      ecs::Entity entity = world.Create();
      Transform transform;
      const Vec3 pos =
          ToWorld(static_cast<f32>(cx) * config_.cell_size, static_cast<f32>(cy) * config_.cell_size,
                  0.0f);
      transform.position[0] = pos.x;
      transform.position[1] = pos.y;
      transform.position[2] = pos.z;
      std::memcpy(transform.rotation, kAxisChange, sizeof(transform.rotation));
      transform.scale = config_.units_to_meters;
      world.Add(entity, transform);
      world.Add(entity, Renderable{built->id});
      ++cells;

      SpawnScatter(world, cx, cy, biome_index);
    }
  }

  RX_INFO("planet tile '{}': {} cells, {} scatter", surface_.name, cells, spawned_scatter_);
  return cells;
}

Vec3 PlanetTile::CameraSpawn() const {
  // Centre of the tile, a bit above the ground.
  const f32 h = GroundHeightAt(0.0f, 0.0f);
  return {0.0f, h + 8.0f, 0.0f};
}

}  // namespace rx::world
