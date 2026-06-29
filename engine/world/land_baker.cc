#include "world/land_baker.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>

#include <base/option.h>

#include "core/log.h"

namespace rec::world {
namespace {

// LTEX texels decoded per 512-unit repeat; the per-cell bake follows at 8x this
// (one repeat = kCellSize / kRepeatUnits). 192 -> 1536x1536 per cell (~12 MB),
// the sweet spot now that the bake is threaded (ParallelRows). 128 -> 1024x1024
// is lighter on VRAM; 256 -> 2048x2048 (~21 MB/cell, ~1 GB over the 49-cell
// radius) is the sharpest the bake reaches. The bake can't match the original
// game's full per-repeat detail (that needs runtime splatting); this just lifts
// the ceiling as far as a per-cell texture sensibly goes. Clamped to [16,256].
base::Option<int> LandBakeTexels{"land.bake.texels", 192, "REC_LAND_BAKE_TEXELS"};

constexpr f32 kCellSize = 4096.0f;
// Land textures repeat every 512 game units (~7.3 m), the same scale the
// terrain mesh UVs used before baking.
constexpr f32 kRepeatUnits = 512.0f;
constexpr u32 kQuadGrid = 17;  // VTXT opacity grid per quadrant

// Runs body(i) for i in [0,count) split across a few threads. The bake's rows
// are independent (each writes a disjoint texel range and only reads the decoded
// layers), so this just divides the work; it keeps high-resolution bakes off the
// streaming thread's critical path.
template <class F>
void ParallelRows(u32 count, F&& body) {
  unsigned hw = std::thread::hardware_concurrency();
  unsigned threads = std::clamp<unsigned>(hw, 1u, 8u);
  if (threads <= 1 || count < 64) {
    for (u32 i = 0; i < count; ++i) body(i);
    return;
  }
  base::Vector<std::thread> pool;
  u32 chunk = (count + threads - 1) / threads;
  for (unsigned t = 0; t < threads; ++t) {
    u32 lo = t * chunk;
    if (lo >= count) break;
    u32 hi = std::min(count, lo + chunk);
    pool.emplace_back([lo, hi, &body] {
      for (u32 i = lo; i < hi; ++i) body(i);
    });
  }
  for (std::thread& th : pool) th.join();
}

constexpr u32 kBtxt = FourCc('B', 'T', 'X', 'T');
constexpr u32 kAtxt = FourCc('A', 'T', 'X', 'T');
constexpr u32 kVtxt = FourCc('V', 'T', 'X', 'T');
constexpr u32 kTnam = FourCc('T', 'N', 'A', 'M');
constexpr u32 kTx00 = FourCc('T', 'X', '0', '0');

f32 SrgbToLinear(u8 v) {
  static const auto table = [] {
    base::Vector<f32> t(256);
    for (u32 i = 0; i < 256; ++i) {
      f32 c = static_cast<f32>(i) / 255.0f;
      t[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
    return t;
  }();
  return table[v];
}

u8 LinearToSrgb(f32 v) {
  // 12-bit LUT: the inverse of SrgbToLinear without a per-texel pow, which is
  // the only transcendental in the bake's inner loop.
  static const auto table = [] {
    base::Vector<u8> t(4096);
    for (u32 i = 0; i < 4096; ++i) {
      f32 c = static_cast<f32>(i) / 4095.0f;
      f32 s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
      t[i] = static_cast<u8>(std::clamp(s, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    return t;
  }();
  v = std::clamp(v, 0.0f, 1.0f);
  return table[static_cast<u32>(v * 4095.0f + 0.5f)];
}

u32 Rgb565To888(u16 c, u8 out[3]) {
  out[0] = static_cast<u8>(((c >> 11) & 0x1f) * 255 / 31);
  out[1] = static_cast<u8>(((c >> 5) & 0x3f) * 255 / 63);
  out[2] = static_cast<u8>((c & 0x1f) * 255 / 31);
  return 0;
}

// Decodes the color half of a BC1/BC3 block (8 bytes) into 4x4 RGB.
void DecodeBc1Colors(const u8* block, bool always_four, u8 out[16][3]) {
  u16 c0, c1;
  std::memcpy(&c0, block, 2);
  std::memcpy(&c1, block + 2, 2);
  u8 palette[4][3];
  Rgb565To888(c0, palette[0]);
  Rgb565To888(c1, palette[1]);
  if (always_four || c0 > c1) {
    for (int k = 0; k < 3; ++k) {
      palette[2][k] = static_cast<u8>((2 * palette[0][k] + palette[1][k]) / 3);
      palette[3][k] = static_cast<u8>((palette[0][k] + 2 * palette[1][k]) / 3);
    }
  } else {
    for (int k = 0; k < 3; ++k) {
      palette[2][k] = static_cast<u8>((palette[0][k] + palette[1][k]) / 2);
      palette[3][k] = 0;
    }
  }
  u32 bits;
  std::memcpy(&bits, block + 4, 4);
  for (u32 i = 0; i < 16; ++i) {
    const u8* c = palette[(bits >> (i * 2)) & 3];
    out[i][0] = c[0];
    out[i][1] = c[1];
    out[i][2] = c[2];
  }
}

size_t MipOffset(const asset::Texture& texture, u32 mip, u32* width, u32* height) {
  bool compressed = texture.format != asset::TextureFormat::kRgba8;
  size_t block = (texture.format == asset::TextureFormat::kBc1 ||
                  texture.format == asset::TextureFormat::kBc4)
                     ? 8
                     : 16;
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

struct QuadLayer {
  u64 ltex = 0;
  u32 quadrant = 0;
  f32 opacity[kQuadGrid * kQuadGrid] = {};
};

// Walks the LAND subrecord stream: BTXT sets a quadrant base LTEX, ATXT opens
// an additive layer whose VTXT opacities follow. Returns true if any texture
// layer was present. Shared by the albedo bake and the splat bake.
bool ParseLandLayers(const bethesda::RecordStore& records, const bethesda::Record& land,
                     u16 land_plugin, u64 base[4], base::Vector<QuadLayer>& layers) {
  bool any = false;
  QuadLayer* open = nullptr;
  for (const bethesda::Subrecord& sub : land.subrecords) {
    if ((sub.type == kBtxt || sub.type == kAtxt) && sub.data.size() >= 8) {
      u32 raw;
      u8 quadrant = sub.data[4];
      std::memcpy(&raw, sub.data.data(), 4);
      if (quadrant > 3) continue;
      u64 ltex = raw == 0 ? 0 : records.ResolveFrom(bethesda::RawFormId{raw}, land_plugin).packed();
      any = true;
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
  return any;
}

}  // namespace

bool LandBaker::DecodeTexture(const asset::Texture& texture, Layer* out) const {
  if (texture.format != asset::TextureFormat::kBc1 &&
      texture.format != asset::TextureFormat::kBc2 &&
      texture.format != asset::TextureFormat::kBc3 &&
      texture.format != asset::TextureFormat::kRgba8) {
    return false;
  }
  // The smallest mip still at or above the sampling resolution.
  u32 mip = 0;
  for (u32 m = 0; m + 1 < texture.mip_count; ++m) {
    if (std::max(texture.width >> (m + 1), texture.height >> (m + 1)) < layer_size_) break;
    mip = m + 1;
  }
  u32 width, height;
  size_t offset = MipOffset(texture, mip, &width, &height);

  base::Vector<u8> rgba(static_cast<size_t>(width) * height * 4);
  if (texture.format == asset::TextureFormat::kRgba8) {
    if (offset + rgba.size() > texture.data.size()) return false;
    std::memcpy(rgba.data(), texture.data.data() + offset, rgba.size());
  } else {
    // BC2 and BC3 share a 16 byte block with the BC1 style color block in the
    // last 8 bytes; only the leading alpha block (ignored here) differs.
    bool alpha_block = texture.format == asset::TextureFormat::kBc2 ||
                       texture.format == asset::TextureFormat::kBc3;
    size_t block_size = alpha_block ? 16 : 8;
    u32 bw = (width + 3) / 4, bh = (height + 3) / 4;
    if (offset + static_cast<size_t>(bw) * bh * block_size > texture.data.size()) return false;
    for (u32 by = 0; by < bh; ++by) {
      for (u32 bx = 0; bx < bw; ++bx) {
        const u8* block = texture.data.data() + offset +
                          (static_cast<size_t>(by) * bw + bx) * block_size;
        u8 colors[16][3];
        DecodeBc1Colors(block + (alpha_block ? 8 : 0), alpha_block, colors);
        for (u32 py = 0; py < 4; ++py) {
          for (u32 px = 0; px < 4; ++px) {
            u32 x = bx * 4 + px, y = by * 4 + py;
            if (x >= width || y >= height) continue;
            u8* dst = rgba.data() + (static_cast<size_t>(y) * width + x) * 4;
            const u8* c = colors[py * 4 + px];
            dst[0] = c[0];
            dst[1] = c[1];
            dst[2] = c[2];
          }
        }
      }
    }
  }

  // Square sampling grid in linear space; non square sources resample by
  // nearest.
  out->size = layer_size_;
  out->rgb.resize(static_cast<size_t>(layer_size_) * layer_size_ * 3);
  for (u32 y = 0; y < layer_size_; ++y) {
    u32 sy = y * height / layer_size_;
    for (u32 x = 0; x < layer_size_; ++x) {
      u32 sx = x * width / layer_size_;
      const u8* src = rgba.data() + (static_cast<size_t>(sy) * width + sx) * 4;
      f32* dst = out->rgb.data() + (static_cast<size_t>(y) * layer_size_ + x) * 3;
      dst[0] = SrgbToLinear(src[0]);
      dst[1] = SrgbToLinear(src[1]);
      dst[2] = SrgbToLinear(src[2]);
    }
  }
  return true;
}

void LandBaker::EnsureBakeSize() {
  if (bake_size_ != 0) return;
  layer_size_ = static_cast<u32>(std::clamp(LandBakeTexels.get(), 16, 256));
  bake_size_ = layer_size_ * static_cast<u32>(kCellSize / kRepeatUnits);  // 8x
}

const LandBaker::Layer* LandBaker::DefaultLayer() {
  EnsureBakeSize();
  if (default_layer_.size == 0) {
    const asset::Texture* texture = assets_.LoadTexture("textures/landscape/tundra01.dds");
    if (!texture || !DecodeTexture(*texture, &default_layer_)) {
      default_layer_.size = 1;
      default_layer_.rgb.resize(3);
      default_layer_.rgb[0] = 0.09f;  // linear tundra-ish
      default_layer_.rgb[1] = 0.10f;
      default_layer_.rgb[2] = 0.05f;
    }
  }
  return &default_layer_;
}

std::string LandBaker::LayerDiffusePath(u64 ltex_packed) const {
  if (ltex_packed == 0) return "textures/landscape/tundra01.dds";
  // LTEX -> TNAM -> TXST -> TX00 diffuse path.
  bethesda::GlobalFormId ltex_id{static_cast<u16>(ltex_packed >> 32),
                                 static_cast<u32>(ltex_packed)};
  bethesda::Record ltex;
  std::string path;
  if (records_.Parse(ltex_id, &ltex)) {
    if (const bethesda::Subrecord* tnam = ltex.Find(kTnam); tnam && tnam->data.size() >= 4) {
      u32 raw;
      std::memcpy(&raw, tnam->data.data(), 4);
      if (const auto* found = records_.Find(ltex_id)) {
        bethesda::GlobalFormId txst_id =
            records_.ResolveFrom(bethesda::RawFormId{raw}, found->winning_plugin);
        bethesda::Record txst;
        if (records_.Parse(txst_id, &txst)) path = txst.GetString(kTx00);
      }
    }
  }
  if (path.empty()) return {};
  path = asset::NormalizePath(path);
  if (!path.starts_with("textures/")) path = "textures/" + path;
  return path;
}

asset::AssetId LandBaker::LayerAsset(u64 ltex_packed) {
  std::string path = LayerDiffusePath(ltex_packed);
  if (path.empty()) path = "textures/landscape/tundra01.dds";
  if (const asset::Texture* texture = assets_.LoadTexture(path)) return texture->id;
  return {};
}

const LandBaker::Layer* LandBaker::LayerFor(u64 ltex_packed) {
  if (ltex_packed == 0) return DefaultLayer();
  if (const Layer* known = layers_.find(ltex_packed)) {
    return known->size != 0 ? known : DefaultLayer();
  }
  Layer* layer = layers_.emplace(ltex_packed).first;
  std::string path = LayerDiffusePath(ltex_packed);
  if (!path.empty()) {
    if (const asset::Texture* texture = assets_.LoadTexture(path)) {
      DecodeTexture(*texture, layer);
    }
  }
  if (layer->size == 0) {
    REC_WARN("land texture missing for ltex {:x}, using default", ltex_packed);
    return DefaultLayer();
  }
  return layer;
}

asset::AssetId LandBaker::BakeAlbedo(const bethesda::Record& land, u16 land_plugin, i16 grid_x,
                                     i16 grid_y) {
  EnsureBakeSize();
  u64 base[4] = {};
  base::Vector<QuadLayer> layers;
  bool any = ParseLandLayers(records_, land, land_plugin, base, layers);
  // Cells without texture layers share one default bake; the cell size is
  // an exact multiple of the repeat so it tiles world-consistently.
  if (!any) {
    if (default_albedo_) return default_albedo_;
    layers.clear();
  }

  // Decode every needed LTEX first; inserting into layers_ can rehash the
  // map, so pointers are only taken once the set is complete.
  for (u32 q = 0; q < 4; ++q) LayerFor(base[q]);
  for (const QuadLayer& layer : layers) LayerFor(layer.ltex);
  auto stable_layer = [&](u64 ltex) -> const Layer* {
    if (ltex == 0) return DefaultLayer();
    const Layer* layer = layers_.find(ltex);
    return layer && layer->size != 0 ? layer : DefaultLayer();
  };
  const Layer* base_layers[4];
  for (u32 q = 0; q < 4; ++q) base_layers[q] = stable_layer(base[q]);
  base::Vector<const Layer*> layer_textures(layers.size());
  for (size_t i = 0; i < layers.size(); ++i) layer_textures[i] = stable_layer(layers[i].ltex);

  // World anchored sample of a decoded layer, tiled every kRepeatUnits.
  auto sample = [&](const Layer& layer, f32 world_x, f32 world_y, f32 out[3]) {
    f32 u = world_x / kRepeatUnits;
    f32 v = -world_y / kRepeatUnits;
    u -= std::floor(u);
    v -= std::floor(v);
    u32 x = std::min(layer.size - 1, static_cast<u32>(u * static_cast<f32>(layer.size)));
    u32 y = std::min(layer.size - 1, static_cast<u32>(v * static_cast<f32>(layer.size)));
    const f32* texel = layer.rgb.data() + (static_cast<size_t>(y) * layer.size + x) * 3;
    out[0] = texel[0];
    out[1] = texel[1];
    out[2] = texel[2];
  };

  asset::Texture texture;
  std::string name = any ? "land/albedo/" + std::to_string(grid_x) + "_" + std::to_string(grid_y)
                         : "land/albedo/default";
  texture.id = asset::MakeAssetId(name);
  texture.format = asset::TextureFormat::kRgba8;
  texture.width = bake_size_;
  texture.height = bake_size_;
  texture.is_srgb = true;
  texture.data.resize(static_cast<size_t>(bake_size_) * bake_size_ * 4);

  // Per quadrant so the inner loop only visits the layers that apply there.
  const u32 kQuadTexels = bake_size_ / 2;
  (void)LinearToSrgb(0.0f);  // warm the LUT before the threads touch it
  for (u32 quadrant = 0; quadrant < 4; ++quadrant) {
    base::Vector<u32> quad_layers;
    for (u32 i = 0; i < layers.size(); ++i) {
      if (layers[i].quadrant == quadrant) quad_layers.push_back(i);
    }
    u32 tx0 = (quadrant & 1) ? kQuadTexels : 0;
    u32 ty0 = (quadrant & 2) ? kQuadTexels : 0;
    ParallelRows(kQuadTexels, [&](u32 qy) {
      u32 ty = ty0 + qy;
      // Texel rows run south to north, matching the terrain mesh V = y/cell.
      f32 ly = (static_cast<f32>(ty) + 0.5f) / bake_size_ * kCellSize;
      f32 world_y = static_cast<f32>(grid_y) * kCellSize + ly;
      f32 gy = (static_cast<f32>(qy) + 0.5f) / kQuadTexels * (kQuadGrid - 1);
      u32 cy = std::min(static_cast<u32>(gy), kQuadGrid - 2);
      f32 fy = gy - static_cast<f32>(cy);
      for (u32 qx = 0; qx < kQuadTexels; ++qx) {
        u32 tx = tx0 + qx;
        f32 lx = (static_cast<f32>(tx) + 0.5f) / bake_size_ * kCellSize;
        f32 world_x = static_cast<f32>(grid_x) * kCellSize + lx;
        f32 gx = (static_cast<f32>(qx) + 0.5f) / kQuadTexels * (kQuadGrid - 1);
        u32 cx = std::min(static_cast<u32>(gx), kQuadGrid - 2);
        f32 fx = gx - static_cast<f32>(cx);

        f32 color[3];
        sample(*base_layers[quadrant], world_x, world_y, color);
        for (u32 i : quad_layers) {
          // Bilinear opacity on the quadrant's 17x17 grid.
          const f32* o = layers[i].opacity;
          f32 o00 = o[cy * kQuadGrid + cx], o10 = o[cy * kQuadGrid + cx + 1];
          f32 o01 = o[(cy + 1) * kQuadGrid + cx], o11 = o[(cy + 1) * kQuadGrid + cx + 1];
          f32 opacity = (o00 * (1 - fx) + o10 * fx) * (1 - fy) + (o01 * (1 - fx) + o11 * fx) * fy;
          if (opacity <= 0.001f) continue;
          f32 layer_color[3];
          sample(*layer_textures[i], world_x, world_y, layer_color);
          for (int k = 0; k < 3; ++k) color[k] += (layer_color[k] - color[k]) * opacity;
        }

        u8* dst = texture.data.data() + (static_cast<size_t>(ty) * bake_size_ + tx) * 4;
        dst[0] = LinearToSrgb(color[0]);
        dst[1] = LinearToSrgb(color[1]);
        dst[2] = LinearToSrgb(color[2]);
        dst[3] = 0xff;
      }
    });
  }

  asset::AssetId id = texture.id;
  assets_.AddTexture(std::move(texture));
  ++baked_;
  if (!any) default_albedo_ = id;
  return id;
}

LandBaker::SplatBake LandBaker::BakeSplat(const bethesda::Record& land, u16 land_plugin,
                                          i16 grid_x, i16 grid_y) {
  u64 base[4] = {};
  base::Vector<QuadLayer> layers;
  ParseLandLayers(records_, land, land_plugin, base, layers);

  // Coverage per LTEX (0 = the default tundra) to pick a 3-texture palette: the
  // four quadrant bases count fully, layers by their mean opacity.
  struct Cov {
    u64 ltex;
    f32 weight;
  };
  base::Vector<Cov> cov;
  auto add_cov = [&](u64 ltex, f32 w) {
    for (Cov& c : cov)
      if (c.ltex == ltex) {
        c.weight += w;
        return;
      }
    cov.push_back({ltex, w});
  };
  for (u32 q = 0; q < 4; ++q) add_cov(base[q], 1.0f);
  for (const QuadLayer& l : layers) {
    f32 sum = 0;
    for (f32 o : l.opacity) sum += o;
    add_cov(l.ltex, sum / (kQuadGrid * kQuadGrid));
  }
  u64 palette[3] = {0, 0, 0};
  f32 best[3] = {-1.0f, -1.0f, -1.0f};
  for (const Cov& c : cov) {
    for (u32 s = 0; s < 3; ++s) {
      if (c.weight > best[s]) {
        for (u32 t = 2; t > s; --t) {
          best[t] = best[t - 1];
          palette[t] = palette[t - 1];
        }
        best[s] = c.weight;
        palette[s] = c.ltex;
        break;
      }
    }
  }
  auto slot_of = [&](u64 ltex) -> u32 {
    for (u32 s = 0; s < 3; ++s)
      if (best[s] >= 0.0f && palette[s] == ltex) return s;
    return 0;  // minor layers fold into the dominant texture
  };

  SplatBake out;
  for (u32 s = 0; s < 3; ++s) out.layers[s] = best[s] >= 0.0f ? LayerAsset(palette[s]) : asset::AssetId{};
  if (!out.layers[0]) return out;  // ok stays false; caller keeps the bake path
  for (u32 s = 1; s < 3; ++s)
    if (!out.layers[s]) out.layers[s] = out.layers[0];

  // Weight map: same BTXT base + ATXT/VTXT compositing as the albedo bake, but
  // accumulating per-palette-slot weights instead of color. Low frequency (the
  // VTXT grid is 17x17 per quadrant), so a small map suffices; the shader tiles
  // the full-resolution layers at the native repeat on top of it.
  constexpr u32 kCtrl = 64;
  constexpr u32 kHalf = kCtrl / 2;
  asset::Texture control;
  control.id = asset::MakeAssetId("land/splat/" + std::to_string(grid_x) + "_" +
                                  std::to_string(grid_y));
  control.format = asset::TextureFormat::kRgba8;
  control.width = kCtrl;
  control.height = kCtrl;
  control.is_srgb = false;  // raw weights, not color
  control.data.resize(static_cast<size_t>(kCtrl) * kCtrl * 4);
  for (u32 ty = 0; ty < kCtrl; ++ty) {
    u32 qy = ty >= kHalf ? ty - kHalf : ty;
    f32 gy = (static_cast<f32>(qy) + 0.5f) / kHalf * (kQuadGrid - 1);
    u32 cy = std::min(static_cast<u32>(gy), kQuadGrid - 2);
    f32 fy = gy - static_cast<f32>(cy);
    for (u32 tx = 0; tx < kCtrl; ++tx) {
      u32 quadrant = (tx >= kHalf ? 1u : 0u) | (ty >= kHalf ? 2u : 0u);
      u32 qx = tx >= kHalf ? tx - kHalf : tx;
      f32 gx = (static_cast<f32>(qx) + 0.5f) / kHalf * (kQuadGrid - 1);
      u32 cx = std::min(static_cast<u32>(gx), kQuadGrid - 2);
      f32 fx = gx - static_cast<f32>(cx);

      f32 w[3] = {0, 0, 0};
      w[slot_of(base[quadrant])] = 1.0f;
      for (const QuadLayer& l : layers) {
        if (l.quadrant != quadrant) continue;
        const f32* o = l.opacity;
        f32 o00 = o[cy * kQuadGrid + cx], o10 = o[cy * kQuadGrid + cx + 1];
        f32 o01 = o[(cy + 1) * kQuadGrid + cx], o11 = o[(cy + 1) * kQuadGrid + cx + 1];
        f32 op = (o00 * (1 - fx) + o10 * fx) * (1 - fy) + (o01 * (1 - fx) + o11 * fx) * fy;
        if (op <= 0.001f) continue;
        u32 s = slot_of(l.ltex);
        for (u32 k = 0; k < 3; ++k) w[k] *= (1 - op);
        w[s] += op;
      }
      u8* dst = control.data.data() + (static_cast<size_t>(ty) * kCtrl + tx) * 4;
      dst[0] = static_cast<u8>(std::clamp(w[0], 0.0f, 1.0f) * 255.0f + 0.5f);
      dst[1] = static_cast<u8>(std::clamp(w[1], 0.0f, 1.0f) * 255.0f + 0.5f);
      dst[2] = static_cast<u8>(std::clamp(w[2], 0.0f, 1.0f) * 255.0f + 0.5f);
      dst[3] = 0xff;
    }
  }
  out.control = control.id;
  assets_.AddTexture(std::move(control));
  ++baked_;
  out.ok = true;
  return out;
}

}  // namespace rec::world
