#include "world/cell_streaming.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/log.h"
#include "world/components.h"

namespace rec::world {
namespace {

constexpr f32 kUnitsToMeters = 0.01428f;
constexpr f32 kCellSize = 4096.0f;
constexpr u32 kLandGridPoints = 33;

constexpr u32 kName = FourCc('N', 'A', 'M', 'E');
constexpr u32 kData = FourCc('D', 'A', 'T', 'A');
constexpr u32 kXscl = FourCc('X', 'S', 'C', 'L');
constexpr u32 kModl = FourCc('M', 'O', 'D', 'L');
constexpr u32 kVhgt = FourCc('V', 'H', 'G', 'T');
constexpr u32 kVnml = FourCc('V', 'N', 'M', 'L');
constexpr u32 kVclr = FourCc('V', 'C', 'L', 'R');
constexpr u32 kDnam = FourCc('D', 'N', 'A', 'M');
constexpr u32 kXclw = FourCc('X', 'C', 'L', 'W');

constexpr u32 kRecordFlagInitiallyDisabled = 0x800;
constexpr u32 kCellFlagHasWater = 0x2;
// XCLW placeholder meaning "use the worldspace default water height".
constexpr f32 kNoCellWater = 3.0e38f;

// The one and only Bethesda -> engine conversion (see the class comment):
// engine = (x, z, -y) * kUnitsToMeters. As a quaternion the axis change is a
// -90 degree rotation about X.
constexpr f32 kAxisChange[4] = {-0.70710678f, 0.0f, 0.0f, 0.70710678f};

Vec3 ToEngine(f32 x, f32 y, f32 z) {
  return {x * kUnitsToMeters, z * kUnitsToMeters, -y * kUnitsToMeters};
}

void QuatMultiply(const f32 a[4], const f32 b[4], f32 out[4]) {
  out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
  out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
  out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
  out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

// REFR rotations are radians about each axis, applied z, then y, then x,
// with the angles negated (the games count clockwise).
void RefrRotationToEngine(const f32 euler[3], f32 out[4]) {
  f32 hx = -euler[0] * 0.5f, hy = -euler[1] * 0.5f, hz = -euler[2] * 0.5f;
  f32 qx[4] = {std::sin(hx), 0, 0, std::cos(hx)};
  f32 qy[4] = {0, std::sin(hy), 0, std::cos(hy)};
  f32 qz[4] = {0, 0, std::sin(hz), std::cos(hz)};
  f32 yz[4], beth[4];
  QuatMultiply(qy, qz, yz);
  QuatMultiply(qx, yz, beth);
  QuatMultiply(kAxisChange, beth, out);
}

u32 CellKey(i16 x, i16 y) {
  return static_cast<u32>(static_cast<u16>(x)) << 16 | static_cast<u16>(y);
}

// VHGT: a float offset then 33x33 i8 deltas, row major from the south west
// corner. Each value is a delta from the previous point in the row; the
// first column accumulates down the rows. Heights are in units of 8.
bool DecodeLandHeights(const bethesda::Record& land, f32 out[kLandGridPoints * kLandGridPoints]) {
  const bethesda::Subrecord* vhgt = land.Find(kVhgt);
  if (!vhgt || vhgt->data.size() < 4 + kLandGridPoints * kLandGridPoints) return false;
  f32 offset;
  std::memcpy(&offset, vhgt->data.data(), 4);
  const i8* deltas = reinterpret_cast<const i8*>(vhgt->data.data() + 4);
  f32 row_start = offset;
  for (u32 r = 0; r < kLandGridPoints; ++r) {
    row_start += static_cast<f32>(deltas[r * kLandGridPoints]);
    f32 value = row_start;
    out[r * kLandGridPoints] = value * 8.0f;
    for (u32 c = 1; c < kLandGridPoints; ++c) {
      value += static_cast<f32>(deltas[r * kLandGridPoints + c]);
      out[r * kLandGridPoints + c] = value * 8.0f;
    }
  }
  return true;
}

}  // namespace

bool CellStreamer::SelectWorldspace(std::string_view editor_id) {
  worldspace_ = records_.FindWorldspace(editor_id);
  if (worldspace_.plugin == 0xffff) {
    REC_ERROR("worldspace not found: {}", editor_id);
    return false;
  }
  grid_ = records_.ExteriorCells(worldspace_);
  if (!grid_) {
    REC_ERROR("worldspace has no exterior cells: {}", editor_id);
    return false;
  }
  EnsureLandMaterial();
  // WRLD DNAM holds the default land and water heights; cells without their
  // own XCLW flood at the water height (Tamriel: -14000, the ocean).
  bethesda::Record wrld;
  if (records_.Parse(worldspace_, &wrld)) {
    if (const bethesda::Subrecord* dnam = wrld.Find(kDnam); dnam && dnam->data.size() >= 8) {
      std::memcpy(&default_water_height_, dnam->data.data() + 4, 4);
    }
  }
  REC_INFO("streaming worldspace {} ({} exterior cells, default water {})", editor_id,
           grid_->size(), default_water_height_);
  return true;
}

void CellStreamer::EnsureLandMaterial() {
  if (land_material_) return;
  asset::Material material;
  material.id = asset::MakeAssetId("land/default");
  material.roughness_factor = 1.0f;
  // Tundra works for the Whiterun plains; vertex colors carry the shading.
  const asset::Texture* texture = assets_.LoadTexture("textures/landscape/tundra01.dds");
  if (texture) {
    material.base_color = texture->id;
  } else {
    material.base_color_factor[0] = 0.32f;
    material.base_color_factor[1] = 0.34f;
    material.base_color_factor[2] = 0.24f;
  }
  assets_.AddMaterial(material);
  land_material_ = material.id;
}

void CellStreamer::Update(ecs::World& world, const Vec3& camera_position) {
  if (!grid_) return;

  // Engine -> Bethesda: x = ex / s, y = -ez / s (height ey is irrelevant).
  f32 beth_x = camera_position.x / kUnitsToMeters;
  f32 beth_y = -camera_position.z / kUnitsToMeters;
  i16 center_x = static_cast<i16>(std::floor(beth_x / kCellSize));
  i16 center_y = static_cast<i16>(std::floor(beth_y / kCellSize));
  i32 radius = settings_.load_radius;

  base::Vector<u32> to_unload;
  for (auto kv : loaded_) {
    i16 x = static_cast<i16>(kv.key >> 16);
    i16 y = static_cast<i16>(kv.key & 0xffff);
    if (std::abs(x - center_x) > radius || std::abs(y - center_y) > radius) {
      to_unload.push_back(kv.key);
    }
  }
  for (u32 key : to_unload) UnloadCell(world, key);

  // Near to far so the ground under the camera appears first.
  u32 mesh_budget = settings_.mesh_budget;
  u32 ref_budget = settings_.ref_budget;
  bool all_done = true;
  for (i32 ring = 0; ring <= radius && mesh_budget > 0 && ref_budget > 0; ++ring) {
    for (i32 dy = -ring; dy <= ring; ++dy) {
      for (i32 dx = -ring; dx <= ring; ++dx) {
        if (std::max(std::abs(dx), std::abs(dy)) != ring) continue;
        i16 x = static_cast<i16>(center_x + dx);
        i16 y = static_cast<i16>(center_y + dy);
        LoadedCell* cell = loaded_.find(CellKey(x, y));
        if (cell && cell->done) continue;
        if (!cell) {
          cell = loaded_.emplace(CellKey(x, y)).first;
          cell->source = grid_->find(bethesda::RecordStore::GridKey(x, y));
        }
        if (!LoadCellIncremental(world, x, y, *cell, mesh_budget, ref_budget)) {
          all_done = false;
          if (mesh_budget == 0 || ref_budget == 0) break;
        }
      }
      if (mesh_budget == 0 || ref_budget == 0) break;
    }
  }

  // Exhausted budgets may have cut the ring walk short of unvisited cells.
  if (mesh_budget == 0 || ref_budget == 0) all_done = false;
  if (all_done && !announced_idle_) {
    announced_idle_ = true;
    REC_INFO(
        "streaming idle: {} cells, {} entities, {} meshes converted, {} refs skipped, "
        "{} land bakes, {} water planes, {} grass instances ({} verts)",
        loaded_.size(), spawned_entities_, base_meshes_.size(), skipped_refs_,
        baker_.baked_count(), water_planes_, grass_baker_.total_instances(),
        grass_baker_.total_vertices());
  } else if (!all_done) {
    announced_idle_ = false;
  }
}

bool CellStreamer::LoadCellIncremental(ecs::World& world, i16 grid_x, i16 grid_y, LoadedCell& cell,
                                       u32& mesh_budget, u32& ref_budget) {
  if (!cell.source) {
    cell.done = true;
    return true;
  }
  if (!cell.terrain_done) {
    if (mesh_budget == 0) return false;
    --mesh_budget;
    SpawnTerrain(world, grid_x, grid_y, cell);
    SpawnWater(world, grid_x, grid_y, cell);
    cell.terrain_done = true;
  }
  if (!cell.grass_done) {
    // The merge (and the one-time GRAS model conversions) costs like a mesh
    // conversion, so it takes a budget slot of its own.
    if (mesh_budget == 0) return false;
    --mesh_budget;
    SpawnGrass(world, grid_x, grid_y, cell);
    cell.grass_done = true;
  }
  while (cell.next_ref < cell.source->refs.size()) {
    if (mesh_budget == 0 || ref_budget == 0) return false;
    u64 ref_id = cell.source->refs[cell.next_ref];
    --ref_budget;
    if (!SpawnReference(world, grid_x, grid_y, ref_id, cell, mesh_budget, false)) {
      // Budget ran out mid-reference; retry the same ref next tick.
      return false;
    }
    ++cell.next_ref;
  }
  cell.done = true;
  REC_DEBUG("cell {},{}: {} refs, {} entities", grid_x, grid_y, cell.source->refs.size(),
            cell.entities.size());
  return true;
}

void CellStreamer::UnloadCell(ecs::World& world, u32 key) {
  LoadedCell* cell = loaded_.find(key);
  if (!cell) return;
  for (ecs::Entity entity : cell->entities) world.Destroy(entity);
  if (physics_) {
    if (cell->terrain_body) physics_->RemoveBody(cell->terrain_body);
    for (physics::BodyId body : cell->bodies) physics_->RemoveBody(body);
  }
  spawned_entities_ -= cell->entities.size();
  loaded_.erase(key);
}

bool CellStreamer::SpawnTerrain(ecs::World& world, i16 grid_x, i16 grid_y, LoadedCell& cell) {
  if (cell.source->land == 0) return false;
  bethesda::GlobalFormId land_id{static_cast<u16>(cell.source->land >> 32),
                                 static_cast<u32>(cell.source->land)};

  std::string mesh_name =
      "land/" + std::to_string(grid_x) + "_" + std::to_string(grid_y);
  asset::AssetId mesh_id = asset::MakeAssetId(mesh_name);
  bethesda::Record land;
  if (!records_.Parse(land_id, &land)) return false;
  f32 heights[kLandGridPoints * kLandGridPoints];
  if (!DecodeLandHeights(land, heights)) return false;
  AddTerrainCollider(grid_x, grid_y, cell, heights);

  const asset::Mesh* mesh = assets_.FindMesh(mesh_id);
  if (!mesh) {
    // Bake the cell's blended albedo; cells without texture layers keep the
    // shared default material.
    asset::AssetId material_id = land_material_;
    asset::AssetId albedo =
        baker_.BakeAlbedo(land, records_.Find(land_id)->winning_plugin, grid_x, grid_y);
    if (albedo) {
      asset::Material material;
      material.id = asset::MakeAssetId(mesh_name + "/material");
      material.base_color = albedo;
      material.roughness_factor = 1.0f;
      assets_.AddMaterial(material);
      material_id = material.id;
    }

    asset::Mesh built;
    built.id = mesh_id;
    built.lods.emplace_back();
    asset::MeshLod& lod = built.lods[0];
    constexpr f32 kSpacing = kCellSize / (kLandGridPoints - 1);
    const bethesda::Subrecord* vnml = land.Find(kVnml);
    const bethesda::Subrecord* vclr = land.Find(kVclr);
    bool has_normals = vnml && vnml->data.size() >= kLandGridPoints * kLandGridPoints * 3;
    bool has_colors = vclr && vclr->data.size() >= kLandGridPoints * kLandGridPoints * 3;

    lod.vertices.reserve(kLandGridPoints * kLandGridPoints);
    f32 min_h = 1e30f, max_h = -1e30f;
    for (u32 r = 0; r < kLandGridPoints; ++r) {
      for (u32 c = 0; c < kLandGridPoints; ++c) {
        u32 i = r * kLandGridPoints + c;
        asset::Vertex v;
        v.position[0] = static_cast<f32>(c) * kSpacing;
        v.position[1] = static_cast<f32>(r) * kSpacing;
        v.position[2] = heights[i];
        min_h = std::min(min_h, heights[i]);
        max_h = std::max(max_h, heights[i]);
        if (has_normals) {
          const i8* n = reinterpret_cast<const i8*>(vnml->data.data() + i * 3);
          f32 length = std::sqrt(static_cast<f32>(n[0]) * n[0] + static_cast<f32>(n[1]) * n[1] +
                                 static_cast<f32>(n[2]) * n[2]);
          if (length > 0) {
            v.normal[0] = n[0] / length;
            v.normal[1] = n[1] / length;
            v.normal[2] = n[2] / length;
          } else {
            v.normal[2] = 1;
          }
        } else {
          v.normal[2] = 1;
        }
        v.tangent[0] = 1;
        v.tangent[3] = 1;
        if (has_colors) {
          const u8* color = vclr->data.data() + i * 3;
          v.color = color[0] | color[1] << 8 | color[2] << 16 | 0xffu << 24;
        }
        // The baked albedo covers the cell exactly; tiling of the source
        // land textures happens inside the bake.
        v.uv[0] = v.position[0] / kCellSize;
        v.uv[1] = v.position[1] / kCellSize;
        lod.vertices.push_back(v);
      }
    }
    lod.indices.reserve((kLandGridPoints - 1) * (kLandGridPoints - 1) * 6);
    for (u32 r = 0; r + 1 < kLandGridPoints; ++r) {
      for (u32 c = 0; c + 1 < kLandGridPoints; ++c) {
        u32 v0 = r * kLandGridPoints + c;
        u32 v1 = v0 + 1;
        u32 v2 = v0 + kLandGridPoints;
        u32 v3 = v2 + 1;
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
    submesh.material = land_material_;
    lod.submeshes.push_back(submesh);
    built.bounds_center[0] = kCellSize * 0.5f;
    built.bounds_center[1] = kCellSize * 0.5f;
    built.bounds_center[2] = (min_h + max_h) * 0.5f;
    built.bounds_radius = std::sqrt(2 * kCellSize * 0.5f * kCellSize * 0.5f +
                                    (max_h - min_h) * 0.5f * (max_h - min_h) * 0.5f);
    mesh = assets_.AddMesh(std::move(built));
  }
  if (!mesh || !EnsureUploaded(*mesh)) return false;

  ecs::Entity entity = world.Create();
  Transform transform;
  Vec3 position = ToEngine(static_cast<f32>(grid_x) * kCellSize,
                           static_cast<f32>(grid_y) * kCellSize, 0.0f);
  transform.position[0] = position.x;
  transform.position[1] = position.y;
  transform.position[2] = position.z;
  std::memcpy(transform.rotation, kAxisChange, sizeof(transform.rotation));
  transform.scale = kUnitsToMeters;
  world.Add(entity, transform);
  world.Add(entity, Renderable{mesh->id});
  world.Add(entity, CellMembership{grid_x, grid_y, false});
  cell.entities.push_back(entity);
  ++spawned_entities_;
  return true;
}

const asset::Mesh* CellStreamer::EnsureWaterMesh() {
  asset::AssetId mesh_id = asset::MakeAssetId("water/cell");
  if (const asset::Mesh* mesh = assets_.FindMesh(mesh_id)) return mesh;

  asset::Material material;
  material.id = asset::MakeAssetId("water/cell/material");
  material.base_color_factor[0] = 0.08f;
  material.base_color_factor[1] = 0.12f;
  material.base_color_factor[2] = 0.16f;
  material.base_color_factor[3] = 0.75f;
  material.metallic_factor = 0;
  material.roughness_factor = 0.05f;
  material.alpha_mode = asset::AlphaMode::kBlend;
  material.two_sided = true;
  material.is_water = true;
  assets_.AddMaterial(material);

  // One cell sized quad in Bethesda space (z up), instanced per flooded cell.
  asset::Mesh built;
  built.id = mesh_id;
  built.lods.emplace_back();
  asset::MeshLod& lod = built.lods[0];
  for (u32 i = 0; i < 4; ++i) {
    asset::Vertex v;
    v.position[0] = (i & 1) ? kCellSize : 0.0f;
    v.position[1] = (i & 2) ? kCellSize : 0.0f;
    v.position[2] = 0.0f;
    v.normal[2] = 1;
    v.tangent[0] = 1;
    v.tangent[3] = 1;
    v.uv[0] = v.position[0] / kCellSize;
    v.uv[1] = v.position[1] / kCellSize;
    lod.vertices.push_back(v);
  }
  for (u32 index : {0u, 1u, 2u, 1u, 3u, 2u}) lod.indices.push_back(index);
  asset::Submesh submesh;
  submesh.index_count = 6;
  submesh.material = material.id;
  lod.submeshes.push_back(submesh);
  built.bounds_center[0] = kCellSize * 0.5f;
  built.bounds_center[1] = kCellSize * 0.5f;
  built.bounds_radius = kCellSize * 0.7072f;
  return assets_.AddMesh(std::move(built));
}

void CellStreamer::AddTerrainCollider(i16 grid_x, i16 grid_y, LoadedCell& cell,
                                      const f32* heights) {
  if (!physics_) return;
  // Bethesda rows run north (+y); engine z runs south, so rows flip. Jolt
  // heightfields need power-of-two-plus... any square sample count works.
  f32 engine_heights[kLandGridPoints * kLandGridPoints];
  for (u32 j = 0; j < kLandGridPoints; ++j) {
    for (u32 i = 0; i < kLandGridPoints; ++i) {
      u32 row = kLandGridPoints - 1 - j;
      engine_heights[j * kLandGridPoints + i] = heights[row * kLandGridPoints + i] * kUnitsToMeters;
    }
  }
  Vec3 origin{static_cast<f32>(grid_x) * kCellSize * kUnitsToMeters, 0,
              -(static_cast<f32>(grid_y) + 1.0f) * kCellSize * kUnitsToMeters};
  cell.terrain_body = physics_->AddHeightField(origin, engine_heights, kLandGridPoints,
                                               kCellSize * kUnitsToMeters);
}

bool CellStreamer::WaterHeightAt(const Vec3& position, f32* height, Vec3* flow) {
  f32 beth_x = position.x / kUnitsToMeters;
  f32 beth_y = -position.z / kUnitsToMeters;
  i16 grid_x = static_cast<i16>(std::floor(beth_x / kCellSize));
  i16 grid_y = static_cast<i16>(std::floor(beth_y / kCellSize));
  const LoadedCell* cell = loaded_.find(CellKey(grid_x, grid_y));
  if (!cell || !cell->source) return false;
  f32 game_height = 0;
  if (!CellWaterHeight(*cell, &game_height)) return false;
  *height = game_height * kUnitsToMeters;

  // Rivers descend cell to cell; the water height gradient between loaded
  // neighbors gives the downstream direction. Lakes are level and stay
  // still. Engine z runs against bethesda y.
  if (flow) {
    *flow = {};
    auto neighbor_height = [&](i16 dx, i16 dy, f32* out) {
      const LoadedCell* neighbor = loaded_.find(CellKey(grid_x + dx, grid_y + dy));
      if (!neighbor || !neighbor->source) return false;
      f32 h = 0;
      if (!CellWaterHeight(*neighbor, &h)) return false;
      *out = h * kUnitsToMeters;
      return true;
    };
    f32 cell_meters = kCellSize * kUnitsToMeters;
    f32 east = *height, west = *height, north = *height, south = *height;
    neighbor_height(1, 0, &east);
    neighbor_height(-1, 0, &west);
    neighbor_height(0, 1, &north);
    neighbor_height(0, -1, &south);
    f32 gradient_x = (east - west) / (2.0f * cell_meters);
    f32 gradient_z = (south - north) / (2.0f * cell_meters);  // z = -bethesda y
    Vec3 downhill{-gradient_x, 0, -gradient_z};
    f32 steepness = std::sqrt(Dot(downhill, downhill));
    if (steepness > 1e-5f) {
      // Flow speed grows with the drop, capped at a brisk current.
      f32 speed = std::min(steepness * 600.0f, 2.5f);
      *flow = downhill * (speed / steepness);
    }
  }
  return true;
}

bool CellStreamer::CellWaterHeight(const LoadedCell& cell, f32* height) const {
  if (cell.source->cell == 0) return false;
  bethesda::Record record;
  if (!records_.Parse({static_cast<u16>(cell.source->cell >> 32),
                       static_cast<u32>(cell.source->cell)},
                      &record)) {
    return false;
  }
  const bethesda::Subrecord* data = record.Find(kData);
  u16 flags = 0;
  if (data && data->data.size() >= 1) {
    flags = data->data[0];
    if (data->data.size() >= 2) flags |= static_cast<u16>(data->data[1]) << 8;
  }
  if (!(flags & kCellFlagHasWater)) return false;

  f32 h = kNoCellWater;
  if (const bethesda::Subrecord* xclw = record.Find(kXclw); xclw && xclw->data.size() >= 4) {
    std::memcpy(&h, xclw->data.data(), 4);
  }
  if (h >= kNoCellWater || std::isnan(h)) h = default_water_height_;
  if (h <= -kNoCellWater) return false;
  *height = h;
  return true;
}

bool CellStreamer::SpawnWater(ecs::World& world, i16 grid_x, i16 grid_y, LoadedCell& cell) {
  f32 height;
  if (!CellWaterHeight(cell, &height)) return false;

  // Skip cells whose terrain sits entirely above the water level.
  if (cell.source->land != 0) {
    bethesda::Record land;
    f32 heights[kLandGridPoints * kLandGridPoints];
    if (records_.Parse({static_cast<u16>(cell.source->land >> 32),
                        static_cast<u32>(cell.source->land)},
                       &land) &&
        DecodeLandHeights(land, heights)) {
      f32 min_h = heights[0];
      for (f32 h : heights) min_h = std::min(min_h, h);
      if (min_h >= height) return false;
    }
  }

  const asset::Mesh* mesh = EnsureWaterMesh();
  if (!mesh || !EnsureUploaded(*mesh)) return false;

  ecs::Entity entity = world.Create();
  Transform transform;
  Vec3 position = ToEngine(static_cast<f32>(grid_x) * kCellSize,
                           static_cast<f32>(grid_y) * kCellSize, height);
  transform.position[0] = position.x;
  transform.position[1] = position.y;
  transform.position[2] = position.z;
  std::memcpy(transform.rotation, kAxisChange, sizeof(transform.rotation));
  transform.scale = kUnitsToMeters;
  world.Add(entity, transform);
  world.Add(entity, Renderable{mesh->id});
  world.Add(entity, CellMembership{grid_x, grid_y, false});
  cell.entities.push_back(entity);
  ++spawned_entities_;
  ++water_planes_;
  return true;
}

bool CellStreamer::SpawnGrass(ecs::World& world, i16 grid_x, i16 grid_y, LoadedCell& cell) {
  if (settings_.grass_density <= 0.0f || cell.source->land == 0) return false;
  bethesda::GlobalFormId land_id{static_cast<u16>(cell.source->land >> 32),
                                 static_cast<u32>(cell.source->land)};
  bethesda::Record land;
  if (!records_.Parse(land_id, &land)) return false;

  // Dry cells read as far above water so only the "above" grasses grow.
  f32 water_height = -kNoCellWater;
  CellWaterHeight(cell, &water_height);

  const asset::Mesh* mesh =
      grass_baker_.BuildCell(land, records_.Find(land_id)->winning_plugin, grid_x, grid_y,
                             water_height, settings_.grass_density);
  if (!mesh || !EnsureUploaded(*mesh)) return false;

  // Same cell-origin transform as the terrain: instances are merged in
  // cell-local Bethesda space.
  ecs::Entity entity = world.Create();
  Transform transform;
  Vec3 position = ToEngine(static_cast<f32>(grid_x) * kCellSize,
                           static_cast<f32>(grid_y) * kCellSize, 0.0f);
  transform.position[0] = position.x;
  transform.position[1] = position.y;
  transform.position[2] = position.z;
  std::memcpy(transform.rotation, kAxisChange, sizeof(transform.rotation));
  transform.scale = kUnitsToMeters;
  world.Add(entity, transform);
  world.Add(entity, Renderable{mesh->id});
  world.Add(entity, CellMembership{grid_x, grid_y, false});
  cell.entities.push_back(entity);
  ++spawned_entities_;
  return true;
}

bool CellStreamer::SpawnReference(ecs::World& world, i16 grid_x, i16 grid_y, u64 ref_id,
                                  LoadedCell& cell, u32& mesh_budget, bool interior) {
  bethesda::GlobalFormId id{static_cast<u16>(ref_id >> 32), static_cast<u32>(ref_id)};
  const bethesda::RecordStore::StoredRecord* stored = records_.Find(id);
  if (!stored) return true;
  if (stored->header.flags & kRecordFlagInitiallyDisabled) return true;

  bethesda::Record refr;
  if (!records_.Parse(id, &refr)) return true;
  const bethesda::Subrecord* name = refr.Find(kName);
  const bethesda::Subrecord* data = refr.Find(kData);
  if (!name || name->data.size() < 4 || !data || data->data.size() < 24) return true;

  u32 base_raw;
  std::memcpy(&base_raw, name->data.data(), 4);
  bethesda::GlobalFormId base_id =
      records_.ResolveFrom(bethesda::RawFormId{base_raw}, stored->winning_plugin);

  bool budget_exceeded = false;
  const asset::Mesh* mesh = MeshForBase(base_id, mesh_budget, budget_exceeded);
  if (budget_exceeded) return false;
  if (!mesh) {
    ++skipped_refs_;
    return true;
  }
  if (!EnsureUploaded(*mesh)) {
    ++skipped_refs_;
    return true;
  }

  f32 placement[6];
  std::memcpy(placement, data->data.data(), 24);
  f32 scale = 1.0f;
  if (const bethesda::Subrecord* xscl = refr.Find(kXscl); xscl && xscl->data.size() >= 4) {
    std::memcpy(&scale, xscl->data.data(), 4);
  }

  ecs::Entity entity = world.Create();
  Transform transform;
  Vec3 position = ToEngine(placement[0], placement[1], placement[2]);
  transform.position[0] = position.x;
  transform.position[1] = position.y;
  transform.position[2] = position.z;
  RefrRotationToEngine(placement + 3, transform.rotation);
  transform.scale = scale * kUnitsToMeters;
  world.Add(entity, transform);
  world.Add(entity, Renderable{mesh->id});
  world.Add(entity, FormLink{id});
  world.Add(entity, CellMembership{grid_x, grid_y, interior});
  cell.entities.push_back(entity);
  ++spawned_entities_;

  // Solid statics only: grass fill and water/blend planes don't collide.
  bool collidable = physics_ && !mesh->exclude_from_rt && !mesh->lods.empty();
  if (collidable) {
    for (const asset::Submesh& submesh : mesh->lods[0].submeshes) {
      const asset::Material* material = assets_.FindMaterial(submesh.material);
      if (material && (material->is_water || material->alpha_mode == asset::AlphaMode::kBlend)) {
        collidable = false;
        break;
      }
    }
  }
  if (collidable) {
    if (physics_->has_mesh_shape(mesh->id.hash) ||
        physics_->RegisterMeshShape(mesh->id.hash, *mesh)) {
      physics::BodyId body = physics_->AddStaticMeshInstance(
          mesh->id.hash, position, transform.rotation, transform.scale);
      if (body) cell.bodies.push_back(body);
    }
  }
  return true;
}

namespace {

// Base record types whose MODL subrecord is a world model path. Other types
// either have no model (markers, sounds) or use MODL differently (ARMO
// stores a form id there).
bool BaseTypeHasWorldModel(u32 type) {
  switch (type) {
    case FourCc('S', 'T', 'A', 'T'):
    case FourCc('T', 'R', 'E', 'E'):
    case FourCc('M', 'S', 'T', 'T'):
    case FourCc('F', 'L', 'O', 'R'):
    case FourCc('F', 'U', 'R', 'N'):
    case FourCc('A', 'C', 'T', 'I'):
    case FourCc('D', 'O', 'O', 'R'):
    case FourCc('C', 'O', 'N', 'T'):
    case FourCc('M', 'I', 'S', 'C'):
    case FourCc('A', 'L', 'C', 'H'):
    case FourCc('I', 'N', 'G', 'R'):
    case FourCc('B', 'O', 'O', 'K'):
    case FourCc('W', 'E', 'A', 'P'):
    case FourCc('A', 'M', 'M', 'O'):
    case FourCc('K', 'E', 'Y', 'M'):
    case FourCc('S', 'L', 'G', 'M'):
    case FourCc('L', 'I', 'G', 'H'):
      return true;
    default:
      return false;
  }
}

}  // namespace

const asset::Mesh* CellStreamer::MeshForBase(bethesda::GlobalFormId base_id, u32& mesh_budget,
                                             bool& budget_exceeded) {
  if (const asset::Mesh* const* known = base_meshes_.find(base_id.packed())) return *known;
  const bethesda::RecordStore::StoredRecord* stored = records_.Find(base_id);
  if (!stored || !BaseTypeHasWorldModel(stored->header.type)) {
    base_meshes_.emplace(base_id.packed(), nullptr);
    return nullptr;
  }
  if (mesh_budget == 0) {
    budget_exceeded = true;
    return nullptr;
  }

  const asset::Mesh* mesh = nullptr;
  bethesda::Record base;
  if (records_.Parse(base_id, &base)) {
    std::string model = base.GetString(kModl);
    if (!model.empty()) {
      std::string path = asset::NormalizePath(model);
      if (!path.starts_with("meshes/")) path = "meshes/" + path;
      // A conversion (or a final failure) is the expensive step we budget.
      if (!assets_.FindMesh(asset::MakeAssetId(path))) --mesh_budget;
      mesh = assets_.LoadMesh(path);
    }
  }
  base_meshes_.emplace(base_id.packed(), mesh);
  return mesh;
}

bool CellStreamer::EnsureUploaded(const asset::Mesh& mesh) {
  if (!uploads_.mesh) return true;  // headless
  if (uploaded_.contains(mesh.id.hash)) return true;

  if (!mesh.lods.empty()) {
    for (const asset::Submesh& submesh : mesh.lods[0].submeshes) {
      const asset::Material* material = assets_.FindMaterial(submesh.material);
      if (!material || uploaded_.contains(material->id.hash)) continue;
      const asset::AssetId textures[] = {material->base_color, material->normal,
                                         material->metallic_roughness, material->emissive};
      for (asset::AssetId texture_id : textures) {
        if (!texture_id || uploaded_.contains(texture_id.hash)) continue;
        if (const asset::Texture* texture = assets_.FindTexture(texture_id)) {
          if (!uploads_.texture(*texture)) REC_WARN("texture upload failed: {:x}", texture_id.hash);
          uploaded_.emplace(texture_id.hash, true);
        } else {
          REC_WARN("texture missing for material {:x}: {:x}", material->id.hash, texture_id.hash);
        }
      }
      if (!uploads_.material(*material)) {
        REC_WARN("material upload failed: {:x}", material->id.hash);
      }
      uploaded_.emplace(material->id.hash, true);
    }
  }
  if (!uploads_.mesh(mesh)) return false;
  uploaded_.emplace(mesh.id.hash, true);
  return true;
}

bool CellStreamer::GroundHeight(f32 engine_x, f32 engine_z, f32* engine_y) const {
  if (!grid_) return false;
  f32 beth_x = engine_x / kUnitsToMeters;
  f32 beth_y = -engine_z / kUnitsToMeters;
  i16 cell_x = static_cast<i16>(std::floor(beth_x / kCellSize));
  i16 cell_y = static_cast<i16>(std::floor(beth_y / kCellSize));
  const bethesda::RecordStore::ExteriorCell* cell =
      grid_->find(bethesda::RecordStore::GridKey(cell_x, cell_y));
  if (!cell || cell->land == 0) return false;

  bethesda::Record land;
  if (!records_.Parse({static_cast<u16>(cell->land >> 32), static_cast<u32>(cell->land)}, &land)) {
    return false;
  }
  f32 heights[kLandGridPoints * kLandGridPoints];
  if (!DecodeLandHeights(land, heights)) return false;

  constexpr f32 kSpacing = kCellSize / (kLandGridPoints - 1);
  f32 local_x = beth_x - static_cast<f32>(cell_x) * kCellSize;
  f32 local_y = beth_y - static_cast<f32>(cell_y) * kCellSize;
  u32 c = std::min(kLandGridPoints - 1, static_cast<u32>(local_x / kSpacing));
  u32 r = std::min(kLandGridPoints - 1, static_cast<u32>(local_y / kSpacing));
  *engine_y = heights[r * kLandGridPoints + c] * kUnitsToMeters;
  return true;
}

bool CellStreamer::LoadInterior(ecs::World& world, bethesda::GlobalFormId cell_id,
                                Vec3* camera_position) {
  const base::Vector<u64>* refs = records_.InteriorRefs(cell_id);
  if (!refs) {
    REC_ERROR("interior cell has no indexed refs: {:04x}:{:06x}", cell_id.plugin,
              cell_id.local_id);
    return false;
  }

  // One shot, unbudgeted: an interior loads completely before the first
  // frame and its entities never unload.
  LoadedCell cell;
  u32 mesh_budget = 0xffffffff;
  for (u64 ref_id : *refs) {
    SpawnReference(world, 0, 0, ref_id, cell, mesh_budget, true);
  }

  // Spawn slightly above the centroid of what was placed.
  Vec3 centroid{};
  u32 count = 0;
  for (ecs::Entity entity : cell.entities) {
    if (const Transform* transform = world.Get<Transform>(entity)) {
      centroid.x += transform->position[0];
      centroid.y += transform->position[1];
      centroid.z += transform->position[2];
      ++count;
    }
  }
  if (count > 0) {
    f32 inv = 1.0f / static_cast<f32>(count);
    centroid.x *= inv;
    centroid.y *= inv;
    centroid.z *= inv;
  }
  centroid.y += 1.5f;
  *camera_position = centroid;

  REC_INFO("interior {:04x}:{:06x}: {} refs, {} entities", cell_id.plugin, cell_id.local_id,
           refs->size(), cell.entities.size());
  return !cell.entities.empty();
}

}  // namespace rec::world
