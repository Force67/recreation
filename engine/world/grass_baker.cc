#include "world/grass_baker.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "core/log.h"

namespace rx::world {
namespace {

constexpr f32 kCellSize = 4096.0f;
constexpr u32 kLandGridPoints = 33;
constexpr u32 kQuadGrid = 17;  // VTXT opacity grid per quadrant
// One spawn roll per ~51x51 game units; with the vanilla tundra densities
// (40-50 of 255) a grassy cell lands around 1.5-2.5k instances.
constexpr u32 kSampleGrid = 80;
constexpr f32 kSampleStep = kCellSize / kSampleGrid;

constexpr u32 kData = FourCc('D', 'A', 'T', 'A');
constexpr u32 kModl = FourCc('M', 'O', 'D', 'L');
constexpr u32 kGnam = FourCc('G', 'N', 'A', 'M');
constexpr u32 kVhgt = FourCc('V', 'H', 'G', 'T');
constexpr u32 kVnml = FourCc('V', 'N', 'M', 'L');
constexpr u32 kBtxt = FourCc('B', 'T', 'X', 'T');
constexpr u32 kAtxt = FourCc('A', 'T', 'X', 'T');
constexpr u32 kVtxt = FourCc('V', 'T', 'X', 'T');

// Deterministic per-cell stream so a reloaded cell regrows identically.
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
};

// Same VHGT decode as the terrain path: float offset then 33x33 i8 deltas,
// heights in units of 8.
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

f32 SampleHeight(const f32* heights, f32 x, f32 y) {
  constexpr f32 kSpacing = kCellSize / (kLandGridPoints - 1);
  f32 gx = std::clamp(x / kSpacing, 0.0f, static_cast<f32>(kLandGridPoints - 1));
  f32 gy = std::clamp(y / kSpacing, 0.0f, static_cast<f32>(kLandGridPoints - 1));
  u32 c = std::min(static_cast<u32>(gx), kLandGridPoints - 2);
  u32 r = std::min(static_cast<u32>(gy), kLandGridPoints - 2);
  f32 fx = gx - static_cast<f32>(c);
  f32 fy = gy - static_cast<f32>(r);
  const f32* row = heights + r * kLandGridPoints + c;
  f32 h0 = row[0] * (1 - fx) + row[1] * fx;
  f32 h1 = row[kLandGridPoints] * (1 - fx) + row[kLandGridPoints + 1] * fx;
  return h0 * (1 - fy) + h1 * fy;
}

// Slope in degrees from the VNML normal nearest to the point. Flat when the
// LAND has no normals.
f32 SlopeDegrees(const bethesda::Subrecord* vnml, f32 x, f32 y) {
  if (!vnml || vnml->data.size() < kLandGridPoints * kLandGridPoints * 3) return 0;
  constexpr f32 kSpacing = kCellSize / (kLandGridPoints - 1);
  u32 c = std::min(kLandGridPoints - 1, static_cast<u32>(std::max(0.0f, x / kSpacing + 0.5f)));
  u32 r = std::min(kLandGridPoints - 1, static_cast<u32>(std::max(0.0f, y / kSpacing + 0.5f)));
  const i8* n = reinterpret_cast<const i8*>(vnml->data.data() + (r * kLandGridPoints + c) * 3);
  f32 length = std::sqrt(static_cast<f32>(n[0]) * n[0] + static_cast<f32>(n[1]) * n[1] +
                         static_cast<f32>(n[2]) * n[2]);
  if (length <= 0) return 0;
  f32 nz = std::clamp(static_cast<f32>(n[2]) / length, -1.0f, 1.0f);
  return std::acos(nz) * 57.29578f;
}

// xEdit "Units From Water Type" semantics; distance is positive above water.
bool WaterOk(u32 type, f32 units, f32 distance) {
  switch (type) {
    case 0: return distance >= units;                       // above - at least
    case 1: return distance >= 0 && distance <= units;      // above - at most
    case 2: return -distance >= units;                      // below - at least
    case 3: return distance <= 0 && -distance <= units;     // below - at most
    case 4: return std::abs(distance) >= units;             // either - at least
    case 5: return std::abs(distance) <= units;             // either - at most
    case 6: return distance <= units;                       // either - at most above
    case 7: return -distance <= units;                      // either - at most below
    default: return true;
  }
}

struct QuadLayer {
  u64 ltex = 0;
  u32 quadrant = 0;
  f32 opacity[kQuadGrid * kQuadGrid] = {};
};

// Bilinear ATXT layer opacity at a quadrant-local position in game units.
f32 LayerOpacity(const QuadLayer& layer, f32 qx, f32 qy) {
  constexpr f32 kQuadSize = kCellSize * 0.5f;
  f32 gx = std::clamp(qx / kQuadSize, 0.0f, 1.0f) * (kQuadGrid - 1);
  f32 gy = std::clamp(qy / kQuadSize, 0.0f, 1.0f) * (kQuadGrid - 1);
  u32 c = std::min(static_cast<u32>(gx), kQuadGrid - 2);
  u32 r = std::min(static_cast<u32>(gy), kQuadGrid - 2);
  f32 fx = gx - static_cast<f32>(c);
  f32 fy = gy - static_cast<f32>(r);
  const f32* o = layer.opacity;
  f32 o0 = o[r * kQuadGrid + c] * (1 - fx) + o[r * kQuadGrid + c + 1] * fx;
  f32 o1 = o[(r + 1) * kQuadGrid + c] * (1 - fx) + o[(r + 1) * kQuadGrid + c + 1] * fx;
  return o0 * (1 - fy) + o1 * fy;
}

}  // namespace

const base::Vector<u64>& GrassBaker::GrassListFor(u64 ltex_packed) {
  if (ltex_packed == 0) return empty_list_;
  if (const base::Vector<u64>* known = ltex_grass_.find(ltex_packed)) return *known;
  base::Vector<u64>* list = ltex_grass_.emplace(ltex_packed).first;

  bethesda::GlobalFormId ltex_id{static_cast<u16>(ltex_packed >> 32),
                                 static_cast<u32>(ltex_packed)};
  bethesda::Record ltex;
  if (records_.Parse(ltex_id, &ltex)) {
    u16 plugin = records_.Find(ltex_id)->winning_plugin;
    for (const bethesda::Subrecord& sub : ltex.subrecords) {
      if (sub.type != kGnam || sub.data.size() < 4) continue;
      u32 raw;
      std::memcpy(&raw, sub.data.data(), 4);
      list->push_back(records_.ResolveFrom(bethesda::RawFormId{raw}, plugin).packed());
    }
  }
  return *list;
}

const GrassBaker::GrassType* GrassBaker::TypeFor(u64 gras_packed) {
  if (const GrassType* known = types_.find(gras_packed)) return known;
  GrassType* type = types_.emplace(gras_packed).first;

  bethesda::GlobalFormId gras_id{static_cast<u16>(gras_packed >> 32),
                                 static_cast<u32>(gras_packed)};
  bethesda::Record gras;
  if (!records_.Parse(gras_id, &gras)) return type;

  // GRAS DATA: u8 density, u8 minSlope, u8 maxSlope, pad, u16 unitsFromWater,
  // pad, u32 unitsFromWaterType, f32 positionRange, heightRange, colorRange,
  // wavePeriod, u8 flags, pad[3].
  if (const bethesda::Subrecord* data = gras.Find(kData); data && data->data.size() >= 32) {
    const u8* d = data->data.data();
    type->density = static_cast<f32>(d[0]) / 255.0f;
    type->min_slope = static_cast<f32>(d[1]);
    type->max_slope = static_cast<f32>(d[2]);
    u16 units;
    std::memcpy(&units, d + 4, 2);
    type->units_from_water = static_cast<f32>(units);
    std::memcpy(&type->water_type, d + 8, 4);
    std::memcpy(&type->position_range, d + 12, 4);
    std::memcpy(&type->height_range, d + 16, 4);
    std::memcpy(&type->color_range, d + 20, 4);
  }

  std::string model = gras.GetString(kModl);
  if (!model.empty()) {
    std::string path = asset::NormalizePath(model);
    if (!path.starts_with("meshes/")) path = "meshes/" + path;
    type->model = assets_.LoadMesh(path);
    if (type->model && type->model->lods.empty()) type->model = nullptr;
    if (!type->model) RX_WARN("grass model failed: {}", path);
    // Flag the model's materials for the renderer's vertex wind (uv.y-weighted
    // sway). Marked here, before EnsureUploaded pushes them to the gpu.
    if (type->model) {
      for (const asset::Submesh& submesh : type->model->lods[0].submeshes) {
        if (asset::Material* m = assets_.FindMaterialMutable(submesh.material)) m->wind = true;
      }
    }
  }
  return type;
}

const asset::Mesh* GrassBaker::BuildCell(const bethesda::Record& land, u16 land_plugin,
                                         i16 grid_x, i16 grid_y, f32 water_height,
                                         f32 density_scale) {
  std::string name = "grass/" + std::to_string(grid_x) + "_" + std::to_string(grid_y);
  asset::AssetId mesh_id = asset::MakeAssetId(name);
  if (const asset::Mesh* cached = assets_.FindMesh(mesh_id)) return cached;

  f32 heights[kLandGridPoints * kLandGridPoints];
  if (!DecodeLandHeights(land, heights)) return nullptr;
  const bethesda::Subrecord* vnml = land.Find(kVnml);

  // Texture layers like the albedo bake: BTXT sets a quadrant base, ATXT
  // opens an additive layer whose VTXT opacities follow.
  u64 base[4] = {};
  base::Vector<QuadLayer> layers;
  QuadLayer* open = nullptr;
  for (const bethesda::Subrecord& sub : land.subrecords) {
    if ((sub.type == kBtxt || sub.type == kAtxt) && sub.data.size() >= 8) {
      u32 raw;
      u8 quadrant = sub.data[4];
      std::memcpy(&raw, sub.data.data(), 4);
      if (quadrant > 3) continue;
      u64 ltex =
          raw == 0 ? 0 : records_.ResolveFrom(bethesda::RawFormId{raw}, land_plugin).packed();
      if (sub.type == kBtxt) {
        base[quadrant] = ltex;
        open = nullptr;
      } else {
        layers.emplace_back();
        open = &layers.back();
        open->ltex = ltex;
        open->quadrant = quadrant;
      }
    } else if (sub.type == kVtxt && open) {
      for (size_t i = 0; i + 8 <= sub.data.size(); i += 8) {
        u16 position;
        f32 opacity;
        std::memcpy(&position, sub.data.data() + i, 2);
        std::memcpy(&opacity, sub.data.data() + i + 4, 4);
        if (position < kQuadGrid * kQuadGrid) {
          open->opacity[position] = std::clamp(opacity, 0.0f, 1.0f);
        }
      }
    }
  }

  // Resolve every LTEX's grass list and every GRAS form up front; the cache
  // maps can rehash while filling, so pointers are only taken once complete.
  for (u32 q = 0; q < 4; ++q) GrassListFor(base[q]);
  for (const QuadLayer& layer : layers) GrassListFor(layer.ltex);
  base::Vector<u64> all_gras;
  auto collect = [&](u64 ltex) {
    for (u64 id : GrassListFor(ltex)) all_gras.push_back(id);
  };
  for (u32 q = 0; q < 4; ++q) collect(base[q]);
  for (const QuadLayer& layer : layers) collect(layer.ltex);
  if (all_gras.empty()) return nullptr;
  for (u64 id : all_gras) TypeFor(id);

  base::UnorderedMap<u64, base::Vector<const GrassType*>> candidates;  // LTEX -> types
  auto resolve = [&](u64 ltex) {
    if (ltex == 0 || candidates.contains(ltex)) return;
    base::Vector<const GrassType*>* list = candidates.emplace(ltex).first;
    if (const base::Vector<u64>* gras_ids = ltex_grass_.find(ltex)) {
      for (u64 id : *gras_ids) {
        const GrassType* type = types_.find(id);
        if (type && type->model) list->push_back(type);
      }
    }
  };
  for (u32 q = 0; q < 4; ++q) resolve(base[q]);
  for (const QuadLayer& layer : layers) resolve(layer.ltex);
  auto candidates_for = [&](u64 ltex) -> const base::Vector<const GrassType*>* {
    return ltex == 0 ? nullptr : candidates.find(ltex);
  };

  asset::Mesh built;
  built.id = mesh_id;
  built.exclude_from_rt = true;
  built.lods.emplace_back();
  asset::MeshLod& lod = built.lods[0];

  // Index lists grouped per source material, concatenated at the end.
  struct Bucket {
    asset::AssetId material;
    base::Vector<u32> indices;
  };
  base::Vector<Bucket> buckets;
  base::UnorderedMap<u64, u32> bucket_by_material;

  Rng rng((static_cast<u64>(static_cast<u16>(grid_x)) << 16 | static_cast<u16>(grid_y)) +
          0x67726173);
  base::Vector<f32> opacities;
  u32 instances = 0;
  f32 min_z = 1e30f, max_z = -1e30f;
  for (u32 r = 0; r < kSampleGrid; ++r) {
    for (u32 c = 0; c < kSampleGrid; ++c) {
      f32 px = (static_cast<f32>(c) + rng.Uniform()) * kSampleStep;
      f32 py = (static_cast<f32>(r) + rng.Uniform()) * kSampleStep;
      u32 quadrant = (px >= kCellSize * 0.5f ? 1u : 0u) | (py >= kCellSize * 0.5f ? 2u : 0u);
      f32 qx = px - (quadrant & 1 ? kCellSize * 0.5f : 0.0f);
      f32 qy = py - (quadrant & 2 ? kCellSize * 0.5f : 0.0f);

      // The dominant texture decides what grows: layers blend in stream
      // order, so layer i contributes o_i * prod(1 - o_j, j > i) and the
      // base what remains. Roads and rock layers painted over grass win
      // here and correctly suppress it.
      const base::Vector<const GrassType*>* best = candidates_for(base[quadrant]);
      opacities.clear();
      for (const QuadLayer& layer : layers) {
        if (layer.quadrant == quadrant) opacities.push_back(LayerOpacity(layer, qx, qy));
      }
      if (!opacities.empty()) {
        f32 best_weight = 1;
        for (f32 opacity : opacities) best_weight *= 1 - opacity;  // base weight
        u32 i = 0;
        for (const QuadLayer& layer : layers) {
          if (layer.quadrant != quadrant) continue;
          f32 weight = opacities[i];
          for (u32 j = i + 1; j < opacities.size(); ++j) weight *= 1 - opacities[j];
          if (weight > best_weight) {
            best_weight = weight;
            best = candidates_for(layer.ltex);
          }
          ++i;
        }
      }
      if (!best || best->empty()) continue;

      for (const GrassType* type : *best) {
        if (rng.Uniform() >= type->density * density_scale) continue;
        f32 slope = SlopeDegrees(vnml, px, py);
        if (slope < type->min_slope || slope > type->max_slope) continue;

        f32 wx = px + (rng.Uniform() * 2 - 1) * type->position_range;
        f32 wy = py + (rng.Uniform() * 2 - 1) * type->position_range;
        f32 wz = SampleHeight(heights, wx, wy) +
                 (rng.Uniform() * 2 - 1) * type->height_range;
        if (!WaterOk(type->water_type, type->units_from_water, wz - water_height)) continue;

        f32 yaw = rng.Uniform() * 6.2831853f;
        f32 scale = 0.8f + rng.Uniform() * 0.4f;
        f32 tint = 1.0f - type->color_range * rng.Uniform();
        f32 cy = std::cos(yaw), sy = std::sin(yaw);

        const asset::MeshLod& src = type->model->lods[0];
        u32 base_vertex = static_cast<u32>(lod.vertices.size());
        for (const asset::Vertex& v : src.vertices) {
          asset::Vertex out = v;
          f32 x = v.position[0] * scale, y = v.position[1] * scale;
          out.position[0] = cy * x - sy * y + wx;
          out.position[1] = sy * x + cy * y + wy;
          out.position[2] = v.position[2] * scale + wz;
          out.normal[0] = cy * v.normal[0] - sy * v.normal[1];
          out.normal[1] = sy * v.normal[0] + cy * v.normal[1];
          out.tangent[0] = cy * v.tangent[0] - sy * v.tangent[1];
          out.tangent[1] = sy * v.tangent[0] + cy * v.tangent[1];
          u32 color = v.color;
          out.color = static_cast<u32>(static_cast<f32>(color & 0xff) * tint) |
                      static_cast<u32>(static_cast<f32>(color >> 8 & 0xff) * tint) << 8 |
                      static_cast<u32>(static_cast<f32>(color >> 16 & 0xff) * tint) << 16 |
                      (color & 0xff000000u);
          lod.vertices.push_back(out);
        }
        for (const asset::Submesh& submesh : src.submeshes) {
          u32* bucket_index = bucket_by_material.find(submesh.material.hash);
          if (!bucket_index) {
            bucket_index =
                bucket_by_material.emplace(submesh.material.hash, static_cast<u32>(buckets.size()))
                    .first;
            buckets.emplace_back();
            buckets.back().material = submesh.material;
          }
          Bucket& bucket = buckets[*bucket_index];
          for (u32 k = 0; k < submesh.index_count; ++k) {
            bucket.indices.push_back(src.indices[submesh.index_offset + k] + base_vertex);
          }
        }
        f32 reach = type->model->bounds_radius * scale;
        min_z = std::min(min_z, wz - reach);
        max_z = std::max(max_z, wz + reach);
        ++instances;
        break;  // one instance per sample point
      }
    }
  }
  if (instances == 0) return nullptr;

  for (Bucket& bucket : buckets) {
    asset::Submesh submesh;
    submesh.index_offset = static_cast<u32>(lod.indices.size());
    submesh.index_count = static_cast<u32>(bucket.indices.size());
    submesh.material = bucket.material;
    for (u32 index : bucket.indices) lod.indices.push_back(index);
    lod.submeshes.push_back(submesh);
  }
  built.bounds_center[0] = kCellSize * 0.5f;
  built.bounds_center[1] = kCellSize * 0.5f;
  built.bounds_center[2] = (min_z + max_z) * 0.5f;
  f32 half_z = (max_z - min_z) * 0.5f;
  built.bounds_radius =
      std::sqrt(2 * kCellSize * 0.5f * kCellSize * 0.5f + half_z * half_z);

  total_instances_ += instances;
  total_vertices_ += lod.vertices.size();
  RX_INFO("grass {},{}: {} instances, {} verts, {} submeshes", grid_x, grid_y, instances,
           lod.vertices.size(), lod.submeshes.size());
  return assets_.AddMesh(std::move(built));
}

}  // namespace rx::world
