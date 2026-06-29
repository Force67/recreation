#include "world/cell_streaming.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#include <base/option.h>

#include "core/log.h"
#include "world/components.h"

namespace rec::world {
namespace {

// Streaming overrides formerly read straight from the environment; populated by
// base::InitOptionsFromEnv() at startup.
base::Option<int> LoadRadius{"load.radius", -1, "REC_LOAD_RADIUS"};
base::Option<bool> DistantLod{"distant.lod", false, "REC_DISTANT_LOD"};
// Runtime terrain splatting: tile the real land textures at native resolution
// and blend by a small weight map, instead of the lower-res per-cell albedo
// bake. On for the higher presets; REC_LAND_SPLAT forces it either way.
base::Option<bool> LandSplat{"land.splat", true, "REC_LAND_SPLAT"};

constexpr f32 kUnitsToMeters = 0.01428f;
constexpr f32 kCellSize = 4096.0f;
constexpr u32 kLandGridPoints = 33;

constexpr u32 kName = FourCc('N', 'A', 'M', 'E');
constexpr u32 kData = FourCc('D', 'A', 'T', 'A');
constexpr u32 kXscl = FourCc('X', 'S', 'C', 'L');
constexpr u32 kAchr = FourCc('A', 'C', 'H', 'R');  // placed actor (NPC) reference
constexpr u32 kVmad = FourCc('V', 'M', 'A', 'D');  // attached script(s)
constexpr u32 kXprm = FourCc('X', 'P', 'R', 'M');  // primitive bound (trigger box)
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
// Distant terrain LOD is sunk this many engine meters so full-detail LAND wins
// the depth test where they overlap (no z-fighting near the camera); negligible
// at horizon range where the LOD is the only ground.
constexpr f32 kDistantTerrainSink = 2.0f;

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
  ground_cache_.clear();  // heights are per worldspace
  worldspace_edid_.assign(editor_id);
  for (char& c : worldspace_edid_) c = static_cast<char>(std::tolower(c));
  distant_quads_.clear();
  distant_entities_.clear();
  distant_next_ = 0;
  distant_discovered_ = false;
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

Vec3 CellStreamer::ToWorld(f32 bethesda_x, f32 bethesda_y, f32 bethesda_z) const {
  Vec3 e = ToEngine(bethesda_x, bethesda_y, bethesda_z);
  return {e.x + world_offset_.x, e.y + world_offset_.y, e.z + world_offset_.z};
}

void CellStreamer::Update(ecs::World& world, const Vec3& camera_position) {
  if (interior_active_ || !grid_) return;

  // The anchor selects which cells load (by this domain's own cell coordinates);
  // world_offset_ then shifts where they spawn, so a secondary worldspace sits
  // beside the primary rather than on top of it. A secondary domain streams a
  // fixed region (a chosen Commonwealth cell) regardless of the shared camera,
  // so its content stays put as a diorama beside the primary world. Engine ->
  // Bethesda: x = ex / s, y = -ez / s (height ey is irrelevant).
  // When streaming around the live camera, undo this domain's world offset first
  // so the anchor lands in the domain's own cell coordinates: zero for the primary
  // game, but a trailer domain recentered onto the shared camera maps its region
  // back to its real cells here.
  const Vec3 anchor = has_fixed_anchor_
                          ? fixed_anchor_
                          : Vec3{camera_position.x - world_offset_.x,
                                 camera_position.y - world_offset_.y,
                                 camera_position.z - world_offset_.z};
  f32 beth_x = anchor.x / kUnitsToMeters;
  f32 beth_y = -anchor.z / kUnitsToMeters;
  i16 center_x = static_cast<i16>(std::floor(beth_x / kCellSize));
  i16 center_y = static_cast<i16>(std::floor(beth_y / kCellSize));
  // REC_LOAD_RADIUS extends the streamed cell ring for greater draw distance;
  // affordable on the mesh-shader lod path (gpu cluster cull + distance lods).
  i32 radius = LoadRadius > 0 ? LoadRadius.get() : settings_.load_radius;

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

  // Distant LOD: build the catalog once, then drain a few quads per tick. These
  // are persistent proxies covering the whole worldspace; the mesh-shader cull
  // (frustum + occlusion) keeps the off-screen / hidden ones free.
  const bool distant_on = DistantLod.overridden() ? DistantLod.get() : settings_.distant_lod;
  if (distant_on) {
    if (!distant_discovered_) DiscoverDistantQuads();
    u32 distant_budget = settings_.distant_budget;
    while (distant_budget > 0 && distant_next_ < distant_quads_.size()) {
      if (SpawnDistantQuad(world, distant_next_)) --distant_budget;
      ++distant_next_;
    }
    if (distant_next_ < distant_quads_.size()) all_done = false;
  }

  // Exhausted budgets may have cut the ring walk short of unvisited cells.
  if (mesh_budget == 0 || ref_budget == 0) all_done = false;
  if (all_done && !announced_idle_) {
    announced_idle_ = true;
    REC_INFO(
        "streaming idle: {} cells, {} entities, {} meshes converted, {} refs skipped, "
        "{} land bakes, {} water planes, {} grass instances ({} verts)",
        loaded_.size(), spawned_entities_, base_meshes_.size(), skipped_refs_, baker_.baked_count(),
        water_planes_, grass_baker_.total_instances(), grass_baker_.total_vertices());
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
  for (ecs::Entity entity : cell->entities) {
    if (quest_world_)
      if (const FormLink* link = world.Get<FormLink>(entity))
        quest_world_->Unregister(link->form.packed());
    world.Destroy(entity);
  }
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

  std::string mesh_name = "land/" + std::to_string(grid_x) + "_" + std::to_string(grid_y);
  asset::AssetId mesh_id = asset::MakeAssetId(mesh_name);
  bethesda::Record land;
  if (!records_.Parse(land_id, &land)) return false;
  f32 heights[kLandGridPoints * kLandGridPoints];
  if (!DecodeLandHeights(land, heights)) return false;
  AddTerrainCollider(grid_x, grid_y, cell, heights);

  const asset::Mesh* mesh = assets_.FindMesh(mesh_id);
  if (!mesh) {
    // Either splat the real land textures at native resolution (preferred) or
    // fall back to the lower-res per-cell albedo bake. Layerless cells keep the
    // shared default material.
    asset::AssetId material_id = land_material_;
    u16 plugin = records_.Find(land_id)->winning_plugin;
    bool splat = LandSplat.overridden() ? LandSplat.get() : settings_.terrain_splat;
    if (splat) {
      LandBaker::SplatBake splat = baker_.BakeSplat(land, plugin, grid_x, grid_y);
      if (splat.ok) {
        asset::Material material;
        material.id = asset::MakeAssetId(mesh_name + "/material");
        // The four texture slots carry three land layers plus the weight map;
        // the shader (kFlagTerrain) tiles and blends them.
        material.base_color = splat.layers[0];
        material.normal = splat.layers[1];
        material.metallic_roughness = splat.layers[2];
        material.emissive = splat.control;
        material.roughness_factor = 1.0f;
        material.is_terrain = true;
        assets_.AddMaterial(material);
        material_id = material.id;
      }
    } else if (asset::AssetId albedo = baker_.BakeAlbedo(land, plugin, grid_x, grid_y)) {
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
    submesh.material = material_id;
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
  Vec3 position =
      ToWorld(static_cast<f32>(grid_x) * kCellSize, static_cast<f32>(grid_y) * kCellSize, 0.0f);
  transform.position[0] = position.x;
  transform.position[1] = position.y;
  transform.position[2] = position.z;
  std::memcpy(transform.rotation, kAxisChange, sizeof(transform.rotation));
  transform.scale = kUnitsToMeters;
  world.Add(entity, transform);
  world.Add(entity, Renderable{RenderMeshId(mesh->id)});
  world.Add(entity, CellMembership{grid_x, grid_y, false});
  cell.entities.push_back(entity);
  ++spawned_entities_;
  return true;
}

namespace {

// Parses "<edid>.<level>.<x>.<y>" (the stem of a .btr/.bto/.btt). Worldspace
// editor ids carry no dots, so the four right-most dot fields are the grid. The
// coarsest (largest) level covers the most cells per quad. Returns false on a
// stem that does not match.
bool ParseLodStem(std::string_view stem, std::string_view edid, i32* level, i32* x, i32* y) {
  if (stem.size() <= edid.size() + 1 || stem.compare(0, edid.size(), edid) != 0 ||
      stem[edid.size()] != '.') {
    return false;
  }
  std::string_view grid = stem.substr(edid.size() + 1);  // "<level>.<x>.<y>"
  i32 vals[3];
  for (i32& v : vals) {
    size_t dot = grid.find('.');
    std::string_view tok = dot == std::string_view::npos ? grid : grid.substr(0, dot);
    if (tok.empty()) return false;
    char* end = nullptr;
    long parsed = std::strtol(std::string(tok).c_str(), &end, 10);
    if (*end != '\0') return false;
    v = static_cast<i32>(parsed);
    grid = dot == std::string_view::npos ? std::string_view() : grid.substr(dot + 1);
  }
  *level = vals[0];
  *x = vals[1];
  *y = vals[2];
  return true;
}

}  // namespace

void CellStreamer::DiscoverDistantQuads() {
  distant_discovered_ = true;
  if (worldspace_edid_.empty()) return;
  const std::string terrain_prefix = "meshes/terrain/" + worldspace_edid_ + "/";
  const std::string object_prefix = terrain_prefix + "objects/";

  // One pass over the vfs collecting every .btr/.bto of this worldspace, keeping
  // each type's coarsest (max) level: those few quads tile the whole map.
  struct Found {
    std::string path;
    i32 level, x, y;
    bool object;
  };
  base::Vector<Found> found;
  i32 max_terrain = -1, max_object = -1;
  assets_.vfs().Enumerate([&](std::string_view path) {
    bool btr = path.ends_with(".btr");
    bool bto = path.ends_with(".bto");
    if (!btr && !bto) return;
    bool object = bto;
    const std::string& prefix = object ? object_prefix : terrain_prefix;
    if (path.size() < prefix.size() || path.compare(0, prefix.size(), prefix) != 0) return;
    std::string_view stem = path.substr(prefix.size());
    stem = stem.substr(0, stem.size() - 4);  // drop ".btr"/".bto"
    i32 level, x, y;
    if (!ParseLodStem(stem, worldspace_edid_, &level, &x, &y)) return;
    found.push_back({std::string(path), level, x, y, object});
    (object ? max_object : max_terrain) = std::max(object ? max_object : max_terrain, level);
  });
  for (const Found& f : found) {
    i32 want = f.object ? max_object : max_terrain;
    if (f.level == want) distant_quads_.push_back({f.path, f.x, f.y, f.object});
  }
  REC_INFO("distant lod: {} quads for {} (terrain lvl {}, object lvl {})", distant_quads_.size(),
           worldspace_edid_, max_terrain, max_object);
}

bool CellStreamer::SpawnDistantQuad(ecs::World& world, size_t index) {
  const DistantQuad& quad = distant_quads_[index];
  // The NIF converter marks .btr/.bto/.btt meshes exclude_from_rt (LOD proxies
  // stay out of the tlas); they carry no collision either.
  const asset::Mesh* mesh = assets_.LoadMesh(quad.path);
  if (!mesh || mesh->lods.empty() || mesh->lods[0].vertices.empty()) return false;
  if (!EnsureUploaded(*mesh)) return false;

  ecs::Entity entity = world.Create();
  Transform transform;
  // .bto object verts are absolute world units (place at the origin); .btr
  // terrain verts are quad-local (place at the quad's SW cell). Sink terrain a
  // touch so the full-detail LAND wins the depth test in the overlap region.
  Vec3 position = quad.object
                      ? ToWorld(0.0f, 0.0f, 0.0f)
                      : ToWorld(static_cast<f32>(quad.cell_x) * kCellSize,
                                static_cast<f32>(quad.cell_y) * kCellSize, 0.0f);
  if (!quad.object) position.y -= kDistantTerrainSink;
  transform.position[0] = position.x;
  transform.position[1] = position.y;
  transform.position[2] = position.z;
  std::memcpy(transform.rotation, kAxisChange, sizeof(transform.rotation));
  transform.scale = kUnitsToMeters;
  world.Add(entity, transform);
  world.Add(entity, Renderable{RenderMeshId(mesh->id)});
  distant_entities_.push_back(entity);
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
  Vec3 origin{static_cast<f32>(grid_x) * kCellSize * kUnitsToMeters + world_offset_.x,
              world_offset_.y,
              -(static_cast<f32>(grid_y) + 1.0f) * kCellSize * kUnitsToMeters + world_offset_.z};
  cell.terrain_body =
      physics_->AddHeightField(origin, engine_heights, kLandGridPoints, kCellSize * kUnitsToMeters);
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
  if (!records_.Parse(
          {static_cast<u16>(cell.source->cell >> 32), static_cast<u32>(cell.source->cell)},
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
    if (records_.Parse(
            {static_cast<u16>(cell.source->land >> 32), static_cast<u32>(cell.source->land)},
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
  Vec3 position =
      ToWorld(static_cast<f32>(grid_x) * kCellSize, static_cast<f32>(grid_y) * kCellSize, height);
  transform.position[0] = position.x;
  transform.position[1] = position.y;
  transform.position[2] = position.z;
  std::memcpy(transform.rotation, kAxisChange, sizeof(transform.rotation));
  transform.scale = kUnitsToMeters;
  world.Add(entity, transform);
  world.Add(entity, Renderable{RenderMeshId(mesh->id)});
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
  Vec3 position =
      ToWorld(static_cast<f32>(grid_x) * kCellSize, static_cast<f32>(grid_y) * kCellSize, 0.0f);
  transform.position[0] = position.x;
  transform.position[1] = position.y;
  transform.position[2] = position.z;
  std::memcpy(transform.rotation, kAxisChange, sizeof(transform.rotation));
  transform.scale = kUnitsToMeters;
  world.Add(entity, transform);
  world.Add(entity, Renderable{RenderMeshId(mesh->id)});
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

  // Placed actors (ACHR) have no static model -- their visuals come from the base
  // NPC's race/skeleton, rendered separately. Create an interactable actor entity
  // from the placement, tagged with its base, and skip the static-mesh path.
  if (stored->header.type == kAchr) {
    f32 placement[6];
    std::memcpy(placement, data->data.data(), 24);
    ecs::Entity entity = world.Create();
    Transform transform;
    Vec3 position = ToWorld(placement[0], placement[1], placement[2]);
    transform.position[0] = position.x;
    transform.position[1] = position.y;
    transform.position[2] = position.z;
    RefrRotationToEngine(placement + 3, transform.rotation);
    world.Add(entity, transform);
    world.Add(entity, FormLink{id});
    world.Add(entity, Npc{base_id});
    world.Add(entity, CellMembership{grid_x, grid_y, interior});
    cell.entities.push_back(entity);
    // Map form -> entity so quests can target this NPC and clients can apply its
    // replicated transform by form id.
    if (quest_world_) quest_world_->Register(id.packed(), entity);
    ++spawned_entities_;
    ++spawned_npcs_;
    return true;
  }

  bool budget_exceeded = false;
  const asset::Mesh* mesh = MeshForBase(base_id, mesh_budget, budget_exceeded);
  if (budget_exceeded) return false;
  if (!mesh) {
    // A meshless reference carrying a script and a primitive bound is a trigger
    // box: keep it as an invisible entity so the interaction layer can fire its
    // OnTriggerEnter when the player walks in (world-driven quest progression).
    if (refr.Find(kXprm) && refr.Find(kVmad)) {
      f32 placement[6];
      std::memcpy(placement, data->data.data(), 24);
      ecs::Entity entity = world.Create();
      Transform transform;
      Vec3 position = ToWorld(placement[0], placement[1], placement[2]);
      transform.position[0] = position.x;
      transform.position[1] = position.y;
      transform.position[2] = position.z;
      RefrRotationToEngine(placement + 3, transform.rotation);
      world.Add(entity, transform);
      world.Add(entity, FormLink{id});
      world.Add(entity, CellMembership{grid_x, grid_y, interior});
      cell.entities.push_back(entity);
      if (quest_world_) quest_world_->Register(id.packed(), entity);
      ++spawned_entities_;
      return true;
    }
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
  Vec3 position = ToWorld(placement[0], placement[1], placement[2]);
  transform.position[0] = position.x;
  transform.position[1] = position.y;
  transform.position[2] = position.z;
  RefrRotationToEngine(placement + 3, transform.rotation);
  transform.scale = scale * kUnitsToMeters;
  world.Add(entity, transform);
  world.Add(entity, Renderable{RenderMeshId(mesh->id)});
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
      physics::BodyId body = physics_->AddStaticMeshInstance(mesh->id.hash, position,
                                                             transform.rotation, transform.scale);
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

ecs::Entity CellStreamer::PlaceObject(ecs::World& world, bethesda::GlobalFormId base_id,
                                      const Vec3& position, const f32 rotation[4], f32 scale,
                                      asset::AssetId* out_mesh) {
  // The editor placed this deliberately, so convert/upload synchronously rather
  // than honoring the streaming budget. MeshForBase caches the result.
  u32 budget = 256;
  bool budget_exceeded = false;
  const asset::Mesh* mesh = MeshForBase(base_id, budget, budget_exceeded);
  if (!mesh || !EnsureUploaded(*mesh)) return ecs::kInvalidEntity;

  ecs::Entity entity = world.Create();
  Transform transform;
  transform.position[0] = position.x;
  transform.position[1] = position.y;
  transform.position[2] = position.z;
  for (int i = 0; i < 4; ++i) transform.rotation[i] = rotation[i];
  // Meshes live in Bethesda object space (1 unit = 1.428 cm); the entity carries
  // the unit->metre scale, so the editor's `scale` is a clean native multiplier.
  transform.scale = scale * kUnitsToMeters;
  world.Add(entity, transform);
  const asset::AssetId render_id = RenderMeshId(mesh->id);
  world.Add(entity, Renderable{render_id});
  if (out_mesh) *out_mesh = render_id;
  return entity;
}

bool CellStreamer::GroundHeight(f32 engine_x, f32 engine_z, f32* engine_y) const {
  if (!grid_) return false;
  f32 beth_x = engine_x / kUnitsToMeters;
  f32 beth_y = -engine_z / kUnitsToMeters;
  i16 cell_x = static_cast<i16>(std::floor(beth_x / kCellSize));
  i16 cell_y = static_cast<i16>(std::floor(beth_y / kCellSize));
  const u32 grid_key = bethesda::RecordStore::GridKey(cell_x, cell_y);
  const bethesda::RecordStore::ExteriorCell* cell = grid_->find(grid_key);
  if (!cell) return false;
  if (cell->land == 0) return RefsGroundHeight(grid_key, *cell, engine_y);

  // Decode the cell's heightfield once and keep it; a placement sweep samples the
  // same few cells hundreds of times per frame.
  base::Vector<f32>* slot = ground_cache_.find(grid_key);
  if (!slot) {
    bethesda::Record land;
    if (!records_.Parse({static_cast<u16>(cell->land >> 32), static_cast<u32>(cell->land)},
                        &land)) {
      return false;
    }
    f32 heights[kLandGridPoints * kLandGridPoints];
    if (!DecodeLandHeights(land, heights)) return false;
    base::Vector<f32> decoded;
    decoded.reserve(kLandGridPoints * kLandGridPoints);
    for (u32 i = 0; i < kLandGridPoints * kLandGridPoints; ++i) decoded.push_back(heights[i]);
    ground_cache_.emplace(grid_key, std::move(decoded));
    slot = ground_cache_.find(grid_key);
    if (!slot) return false;
  }
  const f32* heights = slot->data();

  constexpr f32 kSpacing = kCellSize / (kLandGridPoints - 1);
  f32 local_x = beth_x - static_cast<f32>(cell_x) * kCellSize;
  f32 local_y = beth_y - static_cast<f32>(cell_y) * kCellSize;
  u32 c = std::min(kLandGridPoints - 1, static_cast<u32>(local_x / kSpacing));
  u32 r = std::min(kLandGridPoints - 1, static_cast<u32>(local_y / kSpacing));
  *engine_y = heights[r * kLandGridPoints + c] * kUnitsToMeters;
  return true;
}

bool CellStreamer::RefsGroundHeight(u32 grid_key,
                                    const bethesda::RecordStore::ExteriorCell& cell,
                                    f32* engine_y) const {
  if (const f32* cached = refs_ground_cache_.find(grid_key)) {
    if (std::isnan(*cached)) return false;
    *engine_y = *cached;
    return true;
  }
  // City worldspaces have no heightfield: the floor is the building meshes. Take
  // a low percentile of the placed refs' Z origins as the ground so the camera
  // and player sit on the plaza, not under it. The percentile skips the odd
  // buried prop without snapping up to a tower's base.
  constexpr u32 kData = FourCc('D', 'A', 'T', 'A');
  base::Vector<f32> zs;
  zs.reserve(cell.refs.size());
  for (u64 ref : cell.refs) {
    bethesda::Record record;
    if (!records_.Parse({static_cast<u16>(ref >> 32), static_cast<u32>(ref)}, &record)) continue;
    const bethesda::Subrecord* data = record.Find(kData);
    if (!data || data->data.size() < 24) continue;
    f32 pos[3];
    std::memcpy(pos, data->data.data(), 12);
    zs.push_back(pos[2]);
  }
  if (zs.size() < 8) {  // too sparse to trust an estimate
    refs_ground_cache_.emplace(grid_key, std::numeric_limits<f32>::quiet_NaN());
    return false;
  }
  std::sort(zs.begin(), zs.end());
  const f32 ground = zs[zs.size() / 10] * kUnitsToMeters;
  refs_ground_cache_.emplace(grid_key, ground);
  *engine_y = ground;
  return true;
}

bool CellStreamer::LoadInterior(ecs::World& world, bethesda::GlobalFormId cell_id,
                                Vec3* camera_position) {
  const base::Vector<u64>* refs = records_.InteriorRefs(cell_id);
  if (!refs) {
    REC_ERROR("interior cell has no indexed refs: {:04x}:{:06x}", cell_id.plugin, cell_id.local_id);
    return false;
  }

  // One shot, unbudgeted: an interior loads completely. Its entities are
  // tracked in interior_cell_ so a later transition (a door out) can unload
  // them; exterior streaming stays suspended while it is active.
  interior_cell_ = LoadedCell{};
  interior_active_ = true;
  LoadedCell& cell = interior_cell_;
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

void CellStreamer::UnloadInterior(ecs::World& world) {
  for (ecs::Entity entity : interior_cell_.entities) {
    if (quest_world_)
      if (const FormLink* link = world.Get<FormLink>(entity))
        quest_world_->Unregister(link->form.packed());
    world.Destroy(entity);
  }
  if (physics_)
    for (physics::BodyId body : interior_cell_.bodies) physics_->RemoveBody(body);
  spawned_entities_ -= interior_cell_.entities.size();
  interior_cell_ = LoadedCell{};
}

bool CellStreamer::EnterInterior(ecs::World& world, bethesda::GlobalFormId cell_id,
                                 Vec3* camera_position) {
  // Drop everything currently loaded (exterior cells and any prior interior),
  // then load the destination interior. Collect keys before unloading: the
  // unload erases from loaded_ as it goes.
  base::Vector<u32> keys;
  for (auto kv : loaded_) keys.push_back(kv.key);
  for (u32 key : keys) UnloadCell(world, key);
  UnloadInterior(world);
  announced_idle_ = false;
  const bool ok = LoadInterior(world, cell_id, camera_position);
  if (ok && on_location_change_) on_location_change_(cell_id.packed(), true);
  return ok;
}

void CellStreamer::EnterExterior(ecs::World& world) {
  UnloadInterior(world);
  interior_active_ = false;
  announced_idle_ = false;
  if (on_location_change_) on_location_change_(0, false);
  // The exterior cells were unloaded on the way in; Update re-streams them
  // around the camera on the next tick.
}

void CellStreamer::UnloadAllCells(ecs::World& world) {
  // Everything this streamer spawned: exterior cells, any active interior, and
  // the distant LOD proxies. Collect keys first (UnloadCell erases as it goes).
  base::Vector<u32> keys;
  for (auto kv : loaded_) keys.push_back(kv.key);
  for (u32 key : keys) UnloadCell(world, key);
  if (interior_active_) {
    UnloadInterior(world);
    interior_active_ = false;
  }
  for (ecs::Entity entity : distant_entities_) world.Destroy(entity);
  distant_entities_.clear();
  distant_quads_.clear();
  distant_next_ = 0;
  distant_discovered_ = false;  // re-discovered if this domain becomes active again
  announced_idle_ = false;
}

}  // namespace rec::world
