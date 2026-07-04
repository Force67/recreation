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
// Feed placed LIGH refs (torches, sconces, lamps) into the dynamic light list so
// dungeons and night scenes have local lighting. On by default.
base::Option<bool> PlacedLights{"placed.lights", true, "REC_PLACED_LIGHTS"};
// Project placed TXST refs (blood pools, burn marks, shadowmarks, giant paint)
// through the clustered decal system. On by default.
base::Option<bool> PlacedDecals{"placed.decals", true, "REC_PLACED_DECALS"};

constexpr f32 kUnitsToMeters = 0.01428f;
// Mirrors Renderer::kMaxFrameLights (private): the renderer clamps the bound
// light buffer to this, so past it we keep the streamed lights nearest the camera.
constexpr u32 kMaxFrameLights = 256;
constexpr f32 kCellSize = 4096.0f;
constexpr u32 kLandGridPoints = 33;

constexpr u32 kEdid = FourCc('E', 'D', 'I', 'D');
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
constexpr u32 kXcwt = FourCc('X', 'C', 'W', 'T');  // CELL water type (WATR form id)
constexpr u32 kNam2 = FourCc('N', 'A', 'M', '2');  // WRLD default water (WATR form id)
constexpr u32 kLigh = FourCc('L', 'I', 'G', 'H');  // placed light base record
constexpr u32 kFnam = FourCc('F', 'N', 'A', 'M');  // LIGH fade value (float)
constexpr u32 kXrds = FourCc('X', 'R', 'D', 'S');  // REFR radius override (game units)
constexpr u32 kTxst = FourCc('T', 'X', 'S', 'T');  // texture set (projected decal base)
constexpr u32 kDodt = FourCc('D', 'O', 'D', 'T');  // TXST decal data
constexpr u32 kTx00 = FourCc('T', 'X', '0', '0');  // TXST diffuse path
constexpr u32 kTx01 = FourCc('T', 'X', '0', '1');  // TXST normal path
constexpr u32 kXcll = FourCc('X', 'C', 'L', 'L');  // interior cell lighting
constexpr u32 kLtmp = FourCc('L', 'T', 'M', 'P');  // interior lighting template (LGTM) ref

constexpr u32 kRecordFlagInitiallyDisabled = 0x800;
constexpr u32 kCellFlagHasWater = 0x2;
// XCLW placeholder meaning "use the worldspace default water height".
constexpr f32 kNoCellWater = 3.0e38f;
// Distant terrain LOD is sunk this many engine meters so full-detail LAND wins
// the depth test where they overlap (no z-fighting near the camera); negligible
// at horizon range where the LOD is the only ground.
constexpr f32 kDistantTerrainSink = 2.0f;

// WATR DNAM (Skyrim SE, 228 bytes) byte-RGBA colours, verified against xEdit
// wbDefinitionsSSE and Skyrim.esm: Shallow@40, Deep@44, Reflection@48 (each an
// R,G,B,unused quad). We tint the water plane with the shallow colour. Divided
// by 255 the authored bytes land in the same 0.1..0.3 range the water shader's
// base-colour absorption/scatter tint was hand-tuned for, so the hue changes per
// water type while brightness stays sane (sRGB->linear would over-darken ~4x).
constexpr size_t kWatrShallowColor = 40;

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

// Placed-decal atlas: fixed 256px tiles in a 2048x2048 page (64 tiles), one
// tile per subtexture variant of every decal-capable TXST's diffuse/normal.
constexpr u32 kDecalTile = 256;
constexpr u32 kDecalAtlasSize = 2048;
constexpr u32 kDecalTilesPerRow = kDecalAtlasSize / kDecalTile;
constexpr u32 kDecalMaxTiles = kDecalTilesPerRow * kDecalTilesPerRow;

// Decodes one BC1/BC2/BC3 block to 4x4 RGBA. Unlike the land baker's decoder
// this keeps alpha: it is the decal mask.
void DecodeDecalBlock(const u8* block, asset::TextureFormat format, u8 out[16][4]) {
  const bool alpha_block =
      format == asset::TextureFormat::kBc2 || format == asset::TextureFormat::kBc3;
  const u8* color = block + (alpha_block ? 8 : 0);
  u16 c0, c1;
  std::memcpy(&c0, color, 2);
  std::memcpy(&c1, color + 2, 2);
  u8 palette[4][4];
  auto expand = [](u16 c, u8* rgb) {
    rgb[0] = static_cast<u8>(((c >> 11) & 0x1f) * 255 / 31);
    rgb[1] = static_cast<u8>(((c >> 5) & 0x3f) * 255 / 63);
    rgb[2] = static_cast<u8>((c & 0x1f) * 255 / 31);
  };
  expand(c0, palette[0]);
  expand(c1, palette[1]);
  palette[0][3] = palette[1][3] = palette[2][3] = palette[3][3] = 255;
  if (alpha_block || c0 > c1) {
    for (int k = 0; k < 3; ++k) {
      palette[2][k] = static_cast<u8>((2 * palette[0][k] + palette[1][k]) / 3);
      palette[3][k] = static_cast<u8>((palette[0][k] + 2 * palette[1][k]) / 3);
    }
  } else {
    for (int k = 0; k < 3; ++k) {
      palette[2][k] = static_cast<u8>((palette[0][k] + palette[1][k]) / 2);
      palette[3][k] = 0;
    }
    palette[3][3] = 0;  // BC1 punch-through
  }
  u32 bits;
  std::memcpy(&bits, color + 4, 4);
  for (u32 i = 0; i < 16; ++i) std::memcpy(out[i], palette[(bits >> (i * 2)) & 3], 4);

  if (format == asset::TextureFormat::kBc2) {
    for (u32 i = 0; i < 16; ++i) out[i][3] = static_cast<u8>(((block[i / 2] >> ((i & 1) * 4)) & 0xf) * 17);
  } else if (format == asset::TextureFormat::kBc3) {
    u8 alpha[8] = {block[0], block[1]};
    if (alpha[0] > alpha[1]) {
      for (int k = 2; k < 8; ++k)
        alpha[k] = static_cast<u8>(((8 - k) * alpha[0] + (k - 1) * alpha[1]) / 7);
    } else {
      for (int k = 2; k < 6; ++k)
        alpha[k] = static_cast<u8>(((6 - k) * alpha[0] + (k - 1) * alpha[1]) / 5);
      alpha[6] = 0;
      alpha[7] = 255;
    }
    u64 bits48 = 0;
    std::memcpy(&bits48, block + 2, 6);
    for (u32 i = 0; i < 16; ++i) out[i][3] = alpha[(bits48 >> (i * 3)) & 7];
  }
}

size_t DecalMipOffset(const asset::Texture& texture, u32 mip, u32* width, u32* height) {
  bool compressed = texture.format != asset::TextureFormat::kRgba8;
  size_t block = texture.format == asset::TextureFormat::kBc1 ? 8 : 16;
  size_t offset = 0;
  for (u32 m = 0; m < mip; ++m) {
    u32 w = std::max(1u, texture.width >> m);
    u32 h = std::max(1u, texture.height >> m);
    offset += compressed ? ((w + 3) / 4) * ((h + 3) / 4) * block : static_cast<size_t>(w) * h * 4;
  }
  *width = std::max(1u, texture.width >> mip);
  *height = std::max(1u, texture.height >> mip);
  return offset;
}

// Decodes the smallest mip still at least a tile on its short side to RGBA8.
bool DecodeDecalPixels(const asset::Texture& texture, base::Vector<u8>& rgba, u32* out_w,
                       u32* out_h) {
  if (texture.format != asset::TextureFormat::kBc1 &&
      texture.format != asset::TextureFormat::kBc2 &&
      texture.format != asset::TextureFormat::kBc3 &&
      texture.format != asset::TextureFormat::kRgba8) {
    return false;
  }
  u32 mip = 0;
  for (u32 m = 0; m + 1 < texture.mip_count; ++m) {
    if (std::min(texture.width >> (m + 1), texture.height >> (m + 1)) < kDecalTile) break;
    mip = m + 1;
  }
  u32 width, height;
  size_t offset = DecalMipOffset(texture, mip, &width, &height);
  rgba.resize(static_cast<size_t>(width) * height * 4);
  if (texture.format == asset::TextureFormat::kRgba8) {
    if (offset + rgba.size() > texture.data.size()) return false;
    std::memcpy(rgba.data(), texture.data.data() + offset, rgba.size());
  } else {
    size_t block_size = texture.format == asset::TextureFormat::kBc1 ? 8 : 16;
    u32 bw = (width + 3) / 4, bh = (height + 3) / 4;
    if (offset + static_cast<size_t>(bw) * bh * block_size > texture.data.size()) return false;
    for (u32 by = 0; by < bh; ++by) {
      for (u32 bx = 0; bx < bw; ++bx) {
        u8 texels[16][4];
        DecodeDecalBlock(
            texture.data.data() + offset + (static_cast<size_t>(by) * bw + bx) * block_size,
            texture.format, texels);
        for (u32 py = 0; py < 4; ++py) {
          for (u32 px = 0; px < 4; ++px) {
            u32 x = bx * 4 + px, y = by * 4 + py;
            if (x >= width || y >= height) continue;
            std::memcpy(rgba.data() + (static_cast<size_t>(y) * width + x) * 4, texels[py * 4 + px],
                        4);
          }
        }
      }
    }
  }
  *out_w = width;
  *out_h = height;
  return true;
}

// Appends a full CPU-built mip chain (2x2 box filter) so the upload takes the
// explicit-mips path. Colors weight by alpha so shrinking splats keep their
// hue instead of fading toward the transparent atlas background.
void AppendDecalMips(asset::Texture& texture, bool weight_by_alpha) {
  u32 w = texture.width, h = texture.height;
  size_t src_offset = 0;
  u32 mips = 1;
  while (w > 1 || h > 1) {
    u32 nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
    size_t dst_offset = texture.data.size();
    texture.data.resize(dst_offset + static_cast<size_t>(nw) * nh * 4);
    for (u32 y = 0; y < nh; ++y) {
      for (u32 x = 0; x < nw; ++x) {
        u32 sums[4] = {0, 0, 0, 0};
        u32 weighted[3] = {0, 0, 0};
        for (u32 sy = 0; sy < 2; ++sy) {
          for (u32 sx = 0; sx < 2; ++sx) {
            const u8* s = texture.data.data() + src_offset +
                          (static_cast<size_t>(std::min(y * 2 + sy, h - 1)) * w +
                           std::min(x * 2 + sx, w - 1)) * 4;
            for (int k = 0; k < 4; ++k) sums[k] += s[k];
            for (int k = 0; k < 3; ++k) weighted[k] += s[k] * s[3];
          }
        }
        u8* d = texture.data.data() + dst_offset + (static_cast<size_t>(y) * nw + x) * 4;
        u32 alpha4 = sums[3];
        if (weight_by_alpha && alpha4 > 0) {
          for (int k = 0; k < 3; ++k) d[k] = static_cast<u8>(weighted[k] / alpha4);
        } else {
          for (int k = 0; k < 3; ++k) d[k] = static_cast<u8>(sums[k] / 4);
        }
        d[3] = static_cast<u8>(alpha4 / 4);
      }
    }
    src_offset = dst_offset;
    w = nw;
    h = nh;
    ++mips;
  }
  texture.mip_count = mips;
}

// Nearest-resamples a source region into an atlas tile.
void PackDecalTile(asset::Texture& atlas, u32 tile, const base::Vector<u8>& src, u32 src_w,
                   u32 rx, u32 ry, u32 rw, u32 rh) {
  u32 ox = (tile % kDecalTilesPerRow) * kDecalTile;
  u32 oy = (tile / kDecalTilesPerRow) * kDecalTile;
  for (u32 y = 0; y < kDecalTile; ++y) {
    u32 sy = ry + y * rh / kDecalTile;
    for (u32 x = 0; x < kDecalTile; ++x) {
      u32 sx = rx + x * rw / kDecalTile;
      std::memcpy(atlas.data.data() + (static_cast<size_t>(oy + y) * kDecalAtlasSize + ox + x) * 4,
                  src.data() + (static_cast<size_t>(sy) * src_w + sx) * 4, 4);
    }
  }
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
  // own XCLW flood at the water height (Tamriel: -14000, the ocean). NAM2 is the
  // worldspace default water type (a WATR form), used by cells with no XCWT.
  default_water_form_ = {};
  bethesda::Record wrld;
  if (records_.Parse(worldspace_, &wrld)) {
    if (const bethesda::Subrecord* dnam = wrld.Find(kDnam); dnam && dnam->data.size() >= 8) {
      std::memcpy(&default_water_height_, dnam->data.data() + 4, 4);
    }
    if (const bethesda::Subrecord* nam2 = wrld.Find(kNam2); nam2 && nam2->data.size() >= 4) {
      u32 raw;
      std::memcpy(&raw, nam2->data.data(), 4);
      const bethesda::RecordStore::StoredRecord* ws = records_.Find(worldspace_);
      if (raw != 0 && ws)
        default_water_form_ = records_.ResolveFrom(bethesda::RawFormId{raw}, ws->winning_plugin);
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
  last_camera_ = camera_position;  // tracked even in interiors, for light culling
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

  // Detail rect: the largest ring around the camera cell whose terrain has all
  // spawned. Distant terrain-LOD draws sink their vertices inside it, so the
  // coarse proxy never bridges above the real land (a level-32 quad spanning a
  // valley otherwise cuts through buildings). Cells without LAND stay excluded:
  // there the proxy is the only ground.
  {
    i32 covered = -1;
    for (i32 r = 0; r <= radius; ++r) {
      bool ok = true;
      for (i32 dy = -r; dy <= r && ok; ++dy) {
        for (i32 dx = -r; dx <= r && ok; ++dx) {
          LoadedCell* cell = loaded_.find(
              CellKey(static_cast<i16>(center_x + dx), static_cast<i16>(center_y + dy)));
          if (!cell || !cell->source || !cell->terrain_done) ok = false;
        }
      }
      if (!ok) break;
      covered = r;
    }
    if (covered >= 0) {
      Vec3 a = ToWorld(static_cast<f32>(center_x - covered) * kCellSize,
                       static_cast<f32>(center_y - covered) * kCellSize, 0.0f);
      Vec3 b = ToWorld(static_cast<f32>(center_x + covered + 1) * kCellSize,
                       static_cast<f32>(center_y + covered + 1) * kCellSize, 0.0f);
      detail_rect_[0] = std::min(a.x, b.x);
      detail_rect_[1] = std::min(a.z, b.z);
      detail_rect_[2] = std::max(a.x, b.x);
      detail_rect_[3] = std::max(a.z, b.z);
    } else {
      std::memset(detail_rect_, 0, sizeof(detail_rect_));
    }
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

bethesda::GlobalFormId CellStreamer::ResolveCellWaterForm(const LoadedCell& cell) const {
  if (cell.source && cell.source->cell != 0) {
    bethesda::GlobalFormId cell_id{static_cast<u16>(cell.source->cell >> 32),
                                   static_cast<u32>(cell.source->cell)};
    bethesda::Record record;
    if (records_.Parse(cell_id, &record)) {
      if (const bethesda::Subrecord* xcwt = record.Find(kXcwt); xcwt && xcwt->data.size() >= 4) {
        u32 raw;
        std::memcpy(&raw, xcwt->data.data(), 4);
        const bethesda::RecordStore::StoredRecord* stored = records_.Find(cell_id);
        if (raw != 0 && stored)
          return records_.ResolveFrom(bethesda::RawFormId{raw}, stored->winning_plugin);
      }
    }
  }
  return default_water_form_;  // worldspace NAM2 (invalid when neither resolves)
}

const asset::Mesh* CellStreamer::WaterMeshForCell(const LoadedCell& cell) {
  bethesda::GlobalFormId form = ResolveCellWaterForm(cell);
  u64 key = form.plugin == 0xffff ? 0 : form.packed();
  if (const asset::AssetId* cached = water_meshes_.find(key)) return assets_.FindMesh(*cached);

  // Fallback tint: the historical hardcoded deep blue, kept when no WATR resolves
  // (or its shallow colour is unset).
  f32 tint[3] = {0.08f, 0.12f, 0.16f};
  if (key != 0) {
    bethesda::Record watr;
    if (records_.Parse(form, &watr)) {
      if (const bethesda::Subrecord* dnam = watr.Find(kDnam);
          dnam && dnam->data.size() >= kWatrShallowColor + 3) {
        const u8* c = dnam->data.data() + kWatrShallowColor;
        if (c[0] || c[1] || c[2]) {  // 0,0,0 = unset -> keep the fallback
          for (int i = 0; i < 3; ++i) tint[i] = static_cast<f32>(c[i]) / 255.0f;
          REC_INFO("water: WATR {:04x}:{:06x} {} shallow {},{},{} -> tint {:.3f},{:.3f},{:.3f}",
                   form.plugin, form.local_id, watr.GetString(kEdid), c[0], c[1], c[2], tint[0],
                   tint[1], tint[2]);
        }
      }
    }
  }

  const asset::Mesh* mesh = EnsureWaterMesh(key, tint);
  if (mesh) water_meshes_.emplace(key, mesh->id);
  return mesh;
}

const asset::Mesh* CellStreamer::EnsureWaterMesh(u64 form_key, const f32 tint[3]) {
  std::string base = "water/cell/" + std::to_string(form_key);
  asset::AssetId mesh_id = asset::MakeAssetId(base);
  if (const asset::Mesh* mesh = assets_.FindMesh(mesh_id)) return mesh;

  asset::Material material;
  material.id = asset::MakeAssetId(base + "/material");
  material.base_color_factor[0] = tint[0];
  material.base_color_factor[1] = tint[1];
  material.base_color_factor[2] = tint[2];
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

  const asset::Mesh* mesh = WaterMeshForCell(cell);
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

  // A LIGH ref emits a point light whether or not its base has a world model
  // (many placed lights are meshless emitters), so add it before the mesh path.
  {
    f32 pos[3];
    std::memcpy(pos, data->data.data(), 12);
    const Vec3 position = ToWorld(pos[0], pos[1], pos[2]);
    AddPlacedLight(base_id, refr, position, cell);
    AddPlacedDecal(base_id, id, refr, position, cell);
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

void CellStreamer::AddPlacedLight(bethesda::GlobalFormId base_id, const bethesda::Record& refr,
                                  const Vec3& position, LoadedCell& cell) {
  if (!PlacedLights) return;
  const bethesda::RecordStore::StoredRecord* stored = records_.Find(base_id);
  if (!stored || stored->header.type != kLigh) return;

  bethesda::Record light;
  if (!records_.Parse(base_id, &light)) return;

  render::PointLight l;
  l.pos_radius[0] = position.x;
  l.pos_radius[1] = position.y;
  l.pos_radius[2] = position.z;
  l.pos_radius[3] = 5.0f;  // metres; DATA/XRDS overwrite below
  // Warm default (mirrors the map editor) unless DATA carries a real colour.
  l.color_intensity[0] = 1.0f;
  l.color_intensity[1] = 0.9f;
  l.color_intensity[2] = 0.7f;

  // LIGH DATA: time(i32) radius(u32 @4, game units) colour(rgba @8) ...
  if (const bethesda::Subrecord* d = light.Find(kData); d && d->data.size() >= 12) {
    u32 radius_units;
    std::memcpy(&radius_units, d->data.data() + 4, 4);
    if (radius_units > 0 && radius_units < 20000) l.pos_radius[3] = radius_units * kUnitsToMeters;
    const u8* c = d->data.data() + 8;
    const f32 r = c[0] / 255.0f, g = c[1] / 255.0f, b = c[2] / 255.0f;
    if (r + g + b >= 0.05f) {  // a black record reads as the warm default
      l.color_intensity[0] = r;
      l.color_intensity[1] = g;
      l.color_intensity[2] = b;
    }
  }

  // FNAM fade scales brightness (base 6.0 matches MapEditor); a REFR XRDS is a
  // per-placement radius override in game units.
  f32 intensity = 6.0f;
  if (const bethesda::Subrecord* f = light.Find(kFnam); f && f->data.size() >= 4) {
    f32 fade;
    std::memcpy(&fade, f->data.data(), 4);
    if (fade > 0.0f) intensity *= std::min(fade, 2.0f);
  }
  l.color_intensity[3] = intensity;
  if (const bethesda::Subrecord* x = refr.Find(kXrds); x && x->data.size() >= 4) {
    f32 radius_units;
    std::memcpy(&radius_units, x->data.data(), 4);
    if (radius_units > 0.0f && radius_units < 20000.0f)
      l.pos_radius[3] = radius_units * kUnitsToMeters;
  }

  cell.lights.push_back(l);
}

void CellStreamer::CollectLights(base::Vector<render::PointLight>& out) const {
  if (!PlacedLights) return;

  size_t total = 0;
  for (auto kv : loaded_) total += kv.value.lights.size();
  if (interior_active_) total += interior_cell_.lights.size();

  if (total != logged_light_count_) {
    logged_light_count_ = total;
    REC_INFO("streaming placed lights: {}", total);
  }
  if (total == 0) return;

  // Whatever the caller already appended (editor/demo lights) claims budget first.
  const size_t used = out.size();
  const size_t budget = used < kMaxFrameLights ? kMaxFrameLights - used : 0;
  if (budget == 0) return;

  if (total <= budget) {
    for (auto kv : loaded_)
      for (const render::PointLight& l : kv.value.lights) out.push_back(l);
    if (interior_active_)
      for (const render::PointLight& l : interior_cell_.lights) out.push_back(l);
    return;
  }

  // Over budget: keep the lights nearest the camera.
  base::Vector<const render::PointLight*> all;
  all.reserve(total);
  for (auto kv : loaded_)
    for (const render::PointLight& l : kv.value.lights) all.push_back(&l);
  if (interior_active_)
    for (const render::PointLight& l : interior_cell_.lights) all.push_back(&l);
  const Vec3 cam = last_camera_;
  auto dist2 = [&](const render::PointLight* p) {
    const f32 dx = p->pos_radius[0] - cam.x, dy = p->pos_radius[1] - cam.y,
              dz = p->pos_radius[2] - cam.z;
    return dx * dx + dy * dy + dz * dz;
  };
  std::nth_element(all.begin(), all.begin() + budget, all.end(),
                   [&](const render::PointLight* a, const render::PointLight* b) {
                     return dist2(a) < dist2(b);
                   });
  for (size_t i = 0; i < budget; ++i) out.push_back(*all.begin()[i]);
}

void CellStreamer::EnsureDecalAtlas() {
  if (decal_atlas_built_) return;
  decal_atlas_built_ = true;

  decal_atlas_.id = asset::MakeAssetId("streamed/decals/atlas");
  decal_atlas_.format = asset::TextureFormat::kRgba8;
  decal_atlas_.width = kDecalAtlasSize;
  decal_atlas_.height = kDecalAtlasSize;
  decal_atlas_.is_srgb = true;
  decal_atlas_.data.resize(static_cast<size_t>(kDecalAtlasSize) * kDecalAtlasSize * 4, 0);
  decal_atlas_normal_ = decal_atlas_;
  decal_atlas_normal_.id = asset::MakeAssetId("streamed/decals/atlas_normal");
  decal_atlas_normal_.is_srgb = false;
  for (size_t i = 0; i < decal_atlas_normal_.data.size(); i += 4) {
    decal_atlas_normal_.data[i] = 128;
    decal_atlas_normal_.data[i + 1] = 128;
    decal_atlas_normal_.data[i + 2] = 255;
    decal_atlas_normal_.data[i + 3] = 255;
  }

  // One tile run per distinct diffuse path (TXSTs share the blood sheets).
  struct TileRun {
    u32 tile = 0;
    u32 tiles = 1;
    f32 normal_strength = 0;
  };
  base::UnorderedMap<u64, TileRun> runs;  // keyed by the diffuse path's asset id
  u32 tiles_used = 0;
  base::Vector<u8> pixels, normal_pixels;

  auto texture_path = [](std::string path) {
    path = asset::NormalizePath(path);
    if (!path.empty() && !path.starts_with("textures/")) path = "textures/" + path;
    return path;
  };

  records_.EachOfType(kTxst, [&](bethesda::GlobalFormId id,
                                 const bethesda::RecordStore::StoredRecord&) {
    bethesda::Record txst;
    if (!records_.Parse(id, &txst)) return;
    const bethesda::Subrecord* dodt = txst.Find(kDodt);
    if (!dodt || dodt->data.size() < 36) return;  // not a projected decal
    std::string diffuse = texture_path(txst.GetString(kTx00));
    if (diffuse.empty()) return;

    TileRun run;
    if (const TileRun* known = runs.find(asset::MakeAssetId(diffuse).hash)) {
      run = *known;
    } else {
      const asset::Texture* texture = assets_.LoadTexture(diffuse);
      u32 w = 0, h = 0;
      if (!texture || !DecodeDecalPixels(*texture, pixels, &w, &h)) return;
      // Subtexture sheets lay variants along the long axis; each variant
      // becomes its own square tile.
      u32 subs = 1;
      bool vertical = false;
      if (w >= h * 2) {
        subs = w / h;
      } else if (h >= w * 2) {
        subs = h / w;
        vertical = true;
      }
      if (tiles_used + subs > kDecalMaxTiles) {
        REC_WARN("decal atlas full, skipping {}", diffuse);
        return;
      }
      run.tile = tiles_used;
      run.tiles = subs;
      u32 nw = 0, nh = 0;
      bool has_normal = false;
      if (std::string normal = texture_path(txst.GetString(kTx01)); !normal.empty()) {
        const asset::Texture* normal_texture = assets_.LoadTexture(normal);
        has_normal = normal_texture && DecodeDecalPixels(*normal_texture, normal_pixels, &nw, &nh);
      }
      run.normal_strength = has_normal ? 1.0f : 0.0f;
      for (u32 s = 0; s < subs; ++s) {
        u32 rw = vertical ? w : w / subs, rh = vertical ? h / subs : h;
        PackDecalTile(decal_atlas_, run.tile + s, pixels, w, vertical ? 0 : s * rw,
                      vertical ? s * rh : 0, rw, rh);
        if (has_normal) {
          u32 nrw = vertical ? nw : nw / subs, nrh = vertical ? nh / subs : nh;
          PackDecalTile(decal_atlas_normal_, run.tile + s, normal_pixels, nw,
                        vertical ? 0 : s * nrw, vertical ? s * nrh : 0, nrw, nrh);
        }
      }
      tiles_used += subs;
      runs.insert(asset::MakeAssetId(diffuse).hash, run);
    }

    // DODT: min/max width, min/max height, depth floats (game units), then
    // shininess/parallax and an RGBA tint at offset 32. The game randomizes
    // the size per placement inside min..max; we use the average.
    DecalBase base;
    f32 v[5];
    std::memcpy(v, dodt->data.data(), 20);
    base.half_w = 0.25f * (v[0] + v[1]) * kUnitsToMeters;
    base.half_h = 0.25f * (v[2] + v[3]) * kUnitsToMeters;
    if (base.half_w <= 0.01f || base.half_h <= 0.01f) return;
    // A generous depth floor keeps wide splats attached on uneven terrain.
    base.half_d = std::max(v[4] * kUnitsToMeters, 0.4f);
    const u8* tint = dodt->data.data() + 32;
    base.tint[0] = tint[0] / 255.0f;
    base.tint[1] = tint[1] / 255.0f;
    base.tint[2] = tint[2] / 255.0f;
    base.normal_strength = run.normal_strength;
    base.tile = run.tile;
    base.tiles = run.tiles;
    decal_bases_.insert(id.packed(), base);
  });

  AppendDecalMips(decal_atlas_, /*weight_by_alpha=*/true);
  AppendDecalMips(decal_atlas_normal_, /*weight_by_alpha=*/false);
  if (uploads_.texture) {
    uploads_.texture(decal_atlas_);
    uploads_.texture(decal_atlas_normal_);
  }
  // The pixels live on the GPU now; only the ids stay referenced.
  decal_atlas_.data = base::Vector<u8>{};
  decal_atlas_normal_.data = base::Vector<u8>{};
  ++decal_atlas_version_;
  REC_INFO("decal atlas: {} decal TXST bases, {} tiles", decal_bases_.size(), tiles_used);
}

void CellStreamer::AddPlacedDecal(bethesda::GlobalFormId base_id, bethesda::GlobalFormId ref_id,
                                  const bethesda::Record& refr, const Vec3& position,
                                  LoadedCell& cell) {
  if (!PlacedDecals) return;
  const bethesda::RecordStore::StoredRecord* stored = records_.Find(base_id);
  if (!stored || stored->header.type != kTxst) return;
  EnsureDecalAtlas();
  const DecalBase* base = decal_bases_.find(base_id.packed());
  if (!base) return;

  const bethesda::Subrecord* data = refr.Find(kData);
  if (!data || data->data.size() < 24) return;
  f32 placement[6];
  std::memcpy(placement, data->data.data(), 24);
  f32 scale = 1.0f;
  if (const bethesda::Subrecord* xscl = refr.Find(kXscl); xscl && xscl->data.size() >= 4)
    std::memcpy(&scale, xscl->data.data(), 4);
  if (scale <= 0.0f) scale = 1.0f;

  // The decal quad lies in the base object's local XZ plane and projects along
  // local +Y, so the outward (surface-facing) box axis is local -Y.
  f32 q[4];
  RefrRotationToEngine(placement + 3, q);
  const Quat rotation{q[0], q[1], q[2], q[3]};
  const Vec3 tangent = Rotate(rotation, Vec3{1, 0, 0});
  const Vec3 bitangent = Rotate(rotation, Vec3{0, 0, 1});
  const Vec3 outward = Rotate(rotation, Vec3{0, -1, 0});

  // Authored decal positions hover 1-2 m above the receiving surface (measured
  // against both LAND and placed meshes across sites; the game's projector
  // clearly reaches much further than the DODT depth). Slide the box down its
  // projection axis so it spans from just above the position to a few meters
  // below, instead of a thin authored-depth slab that misses the surface.
  const f32 reach = std::max(base->half_d * scale, 1.5f);
  const Vec3 center = position - outward * reach;
  const f32 half_depth = reach + base->half_d * scale;

  render::Decal d;
  auto row = [&](const Vec3& axis, f32 extent, f32* out) {
    out[0] = axis.x / extent;
    out[1] = axis.y / extent;
    out[2] = axis.z / extent;
    out[3] = -(axis.x * center.x + axis.y * center.y + axis.z * center.z) / extent;
  };
  row(tangent, base->half_w * scale, d.row0);
  row(bitangent, base->half_h * scale, d.row1);
  row(outward, half_depth, d.row2);

  // Subtexture sheets pick a stable per-placement variant.
  u32 tile = base->tile;
  if (base->tiles > 1) tile += static_cast<u32>(ref_id.packed() % base->tiles);
  const f32 tile_uv = static_cast<f32>(kDecalTile) / kDecalAtlasSize;
  d.uv_rect[0] = tile_uv;
  d.uv_rect[1] = tile_uv;
  d.uv_rect[2] = static_cast<f32>(tile % kDecalTilesPerRow) * tile_uv;
  d.uv_rect[3] = static_cast<f32>(tile / kDecalTilesPerRow) * tile_uv;
  d.tint_blend[0] = base->tint[0];
  d.tint_blend[1] = base->tint[1];
  d.tint_blend[2] = base->tint[2];
  d.tint_blend[3] = 1.0f;
  d.params2[0] = base->normal_strength;
  cell.decals.push_back({d, position});
}

void CellStreamer::CollectDecals(base::Vector<render::Decal>& out) const {
  if (!PlacedDecals) return;

  size_t total = 0;
  for (auto kv : loaded_) total += kv.value.decals.size();
  if (interior_active_) total += interior_cell_.decals.size();

  if (total != logged_decal_count_) {
    logged_decal_count_ = total;
    REC_INFO("streaming placed decals: {}", total);
  }
  if (total == 0) return;

  const size_t used = out.size();
  const size_t budget = used < render::kMaxFrameDecals ? render::kMaxFrameDecals - used : 0;
  if (budget == 0) return;

  if (total <= budget) {
    for (auto kv : loaded_)
      for (const LoadedCell::PlacedDecal& d : kv.value.decals) out.push_back(d.decal);
    if (interior_active_)
      for (const LoadedCell::PlacedDecal& d : interior_cell_.decals) out.push_back(d.decal);
    return;
  }

  // Over budget: keep the decals nearest the camera.
  base::Vector<const LoadedCell::PlacedDecal*> all;
  all.reserve(total);
  for (auto kv : loaded_)
    for (const LoadedCell::PlacedDecal& d : kv.value.decals) all.push_back(&d);
  if (interior_active_)
    for (const LoadedCell::PlacedDecal& d : interior_cell_.decals) all.push_back(&d);
  const Vec3 cam = last_camera_;
  auto dist2 = [&](const LoadedCell::PlacedDecal* p) {
    const f32 dx = p->position.x - cam.x, dy = p->position.y - cam.y,
              dz = p->position.z - cam.z;
    return dx * dx + dy * dy + dz * dz;
  };
  std::nth_element(all.begin(), all.begin() + budget, all.end(),
                   [&](const LoadedCell::PlacedDecal* a, const LoadedCell::PlacedDecal* b) {
                     return dist2(a) < dist2(b);
                   });
  for (size_t i = 0; i < budget; ++i) out.push_back(all.begin()[i]->decal);
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
  // Particle-emitter effect textures are not submesh materials, so upload them
  // here (before the mesh upload resolves each to a bindless index).
  for (const asset::ParticleEmitter& emitter : mesh.emitters) {
    if (emitter.texture == 0 || uploaded_.contains(emitter.texture)) continue;
    if (const asset::Texture* texture = assets_.FindTexture(asset::AssetId{emitter.texture})) {
      if (!uploads_.texture(*texture)) REC_WARN("emitter texture upload failed: {:x}", emitter.texture);
    }
    uploaded_.emplace(emitter.texture, true);
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

void CellStreamer::ResolveInteriorLighting(bethesda::GlobalFormId cell_id) {
  interior_lighting_ = InteriorLighting{};
  bethesda::Record cell;
  if (!records_.Parse(cell_id, &cell)) return;
  const bethesda::Subrecord* xcll = cell.Find(kXcll);

  // The XCLL 'Lighting' block and the LGTM DATA block share the first 80 bytes:
  // ambient/directional/fog-near byte colours, fog near/far floats, directional
  // rotation xy/z, directional fade, fog clip, fog power, the 32-byte directional
  // ambient (DALC) block, fog-far colour and fog max. XCLL then carries a u32
  // inherit-flag field at offset 88. Layout per xEdit wbDefinitionsTES5.pas.
  struct Block {
    Vec3 ambient{}, directional{}, fog_near_color{}, fog_far_color{};
    f32 fog_near = 0, fog_far = 0, dir_fade = 0, fog_pow = 1, fog_max = 1;
    i32 rot_xy = 0, rot_z = 0;
    bool has = false;
  };
  auto rgb = [](const u8* d) { return Vec3{d[0] / 255.0f, d[1] / 255.0f, d[2] / 255.0f}; };
  auto parse = [&](const bethesda::Subrecord* sub) {
    Block b;
    if (!sub || sub->data.size() < 40) return b;
    const u8* d = sub->data.data();
    const size_t n = sub->data.size();
    b.has = true;
    b.ambient = rgb(d);
    b.directional = rgb(d + 4);
    b.fog_near_color = rgb(d + 8);
    std::memcpy(&b.fog_near, d + 12, 4);
    std::memcpy(&b.fog_far, d + 16, 4);
    std::memcpy(&b.rot_xy, d + 20, 4);
    std::memcpy(&b.rot_z, d + 24, 4);
    std::memcpy(&b.dir_fade, d + 28, 4);
    std::memcpy(&b.fog_pow, d + 36, 4);
    if (n >= 80) {
      b.fog_far_color = rgb(d + 72);
      std::memcpy(&b.fog_max, d + 76, 4);
    } else {
      b.fog_far_color = b.fog_near_color;
    }
    return b;
  };

  Block cellb = parse(xcll);
  Block tmpl;
  if (const bethesda::Subrecord* ltmp = cell.Find(kLtmp); ltmp && ltmp->data.size() >= 4) {
    u32 raw;
    std::memcpy(&raw, ltmp->data.data(), 4);
    const bethesda::RecordStore::StoredRecord* stored = records_.Find(cell_id);
    bethesda::GlobalFormId tid =
        records_.ResolveFrom(bethesda::RawFormId{raw}, stored ? stored->winning_plugin : 0);
    bethesda::Record lgtm;
    if (records_.Parse(tid, &lgtm)) tmpl = parse(lgtm.Find(kData));
  }

  if (!cellb.has && !tmpl.has) return;  // no authored lighting, keep the defaults

  // Inherit flags: a set bit takes the template's value for that group, a clear
  // bit the cell's own. A cell with no XCLL inherits every group from its template.
  u32 inherit = 0;
  if (cellb.has && xcll && xcll->data.size() >= 92)
    std::memcpy(&inherit, xcll->data.data() + 88, 4);
  else if (!cellb.has)
    inherit = 0x7ff;
  const bool t = tmpl.has;
  auto pick3 = [&](u32 bit, const Vec3& c, const Vec3& tv) {
    return (t && (inherit & bit)) ? tv : c;
  };
  auto pickf = [&](u32 bit, f32 c, f32 tv) { return (t && (inherit & bit)) ? tv : c; };
  auto picki = [&](u32 bit, i32 c, i32 tv) { return (t && (inherit & bit)) ? tv : c; };

  // The byte colours are sRGB. The fog and directional feed the linear HDR scene
  // (fog blends into it, the directional rides the linear sun path), so those get
  // sRGB->linear; the ambient stays gamma as a direct albedo multiplier, which
  // keeps interiors readably lit rather than crushing them near black.
  auto to_linear = [](const Vec3& c) {
    return Vec3{std::pow(c.x, 2.2f), std::pow(c.y, 2.2f), std::pow(c.z, 2.2f)};
  };
  InteriorLighting& L = interior_lighting_;
  L.valid = true;
  L.ambient = pick3(0x001, cellb.ambient, tmpl.ambient);
  L.directional_color = to_linear(pick3(0x002, cellb.directional, tmpl.directional));
  L.fog_near_color = to_linear(pick3(0x004, cellb.fog_near_color, tmpl.fog_near_color));
  L.fog_far_color = to_linear(pick3(0x004, cellb.fog_far_color, tmpl.fog_far_color));
  const f32 near_units = pickf(0x008, cellb.fog_near, tmpl.fog_near);
  const f32 far_units = pickf(0x010, cellb.fog_far, tmpl.fog_far);
  const i32 rot_xy = picki(0x020, cellb.rot_xy, tmpl.rot_xy);
  const f32 dir_fade = pickf(0x040, cellb.dir_fade, tmpl.dir_fade);
  const f32 fog_pow = pickf(0x100, cellb.fog_pow, tmpl.fog_pow);
  const f32 fog_max = pickf(0x200, cellb.fog_max, tmpl.fog_max);

  L.fog_near = near_units * kUnitsToMeters;
  L.fog_far = far_units * kUnitsToMeters;
  L.fog_power = fog_pow > 0.01f ? fog_pow : 1.0f;
  L.fog_max = fog_max > 0.0f ? std::min(fog_max, 1.0f) : 1.0f;

  // Directional fill from XCLL, driven through the sun path. The rotation encodes
  // a direction; take rot_xy as azimuth and bias it downward so it reads as an
  // overhead fill (the colour is dim, so exactness is not critical).
  const f32 az = static_cast<f32>(rot_xy) * 3.14159265f / 180.0f;
  const Vec3 beth = Normalize(Vec3{std::sin(az), std::cos(az), -1.2f});
  L.directional_dir = Normalize(Vec3{beth.x, beth.z, -beth.y});
  const f32 lum = L.directional_color.x + L.directional_color.y + L.directional_color.z;
  L.directional_intensity = lum > 0.001f ? (3.0f + dir_fade) : 0.0f;

  REC_INFO("interior lighting {:04x}:{:06x}: ambient ({:.2f},{:.2f},{:.2f}) directional "
           "({:.2f},{:.2f},{:.2f}) i={:.1f} fog {:.1f}..{:.1f}m col ({:.2f},{:.2f},{:.2f}) "
           "pow {:.2f} max {:.2f}",
           cell_id.plugin, cell_id.local_id, L.ambient.x, L.ambient.y, L.ambient.z,
           L.directional_color.x, L.directional_color.y, L.directional_color.z,
           L.directional_intensity, L.fog_near, L.fog_far, L.fog_near_color.x,
           L.fog_near_color.y, L.fog_near_color.z, L.fog_power, L.fog_max);
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
  ResolveInteriorLighting(cell_id);
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
  interior_lighting_ = InteriorLighting{};
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
    interior_lighting_ = InteriorLighting{};
  }
  for (ecs::Entity entity : distant_entities_) world.Destroy(entity);
  distant_entities_.clear();
  distant_quads_.clear();
  distant_next_ = 0;
  distant_discovered_ = false;  // re-discovered if this domain becomes active again
  announced_idle_ = false;
}

}  // namespace rec::world
