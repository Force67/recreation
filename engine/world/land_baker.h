#ifndef RECREATION_WORLD_LAND_BAKER_H_
#define RECREATION_WORLD_LAND_BAKER_H_

#include <string>

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "asset/asset_database.h"
#include "bethesda/load_order.h"
#include "bethesda/record.h"
#include "core/types.h"

namespace rx::world {

// Bakes one albedo texture per exterior cell from the LAND texture layers:
// BTXT picks the base LTEX per quadrant, ATXT/VTXT stack additive layers
// with per-vertex opacities on a 17x17 grid per quadrant. The LTEX diffuse
// textures (via TXST) are sampled on the CPU at a small mip, world anchored
// so neighboring cells line up. The result uploads as a plain srgb RGBA8
// texture; vertex colors (VCLR) keep multiplying in the shader.
class LandBaker {
 public:
  LandBaker(const bethesda::RecordStore& records, asset::AssetDatabase& assets)
      : records_(records), assets_(assets) {}

  // Returns the baked texture id, or a zero id when the LAND record carries
  // no texture layers (caller keeps its default land material).
  asset::AssetId BakeAlbedo(const bethesda::Record& land, u16 land_plugin, i16 grid_x,
                            i16 grid_y);

  // Runtime-splat inputs for a cell: up to three full-resolution land textures
  // (the most-covered LTEX) plus a small weight map blending them. The shader
  // tiles the layers at the native land repeat, so detail is bounded by the
  // source textures, not a per-cell bake. layers[] always point at a valid
  // texture (slots collapse to the dominant one when the cell has fewer).
  struct SplatBake {
    asset::AssetId layers[3];
    asset::AssetId control;
    bool ok = false;
  };
  SplatBake BakeSplat(const bethesda::Record& land, u16 land_plugin, i16 grid_x, i16 grid_y);

  // Splat v2: a per-cell palette of up to 8 LTEX layers (coverage-ranked, so
  // most cells keep every layer Skyrim authored instead of the top 3) blended
  // by two RGBA8 weight maps (palette slots 0-3 / 4-7; weights renormalize in
  // the shader, so bilinear filtering stays valid - never store indices in a
  // filtered map). Each layer's normal map is resolved from its texture set
  // (zero id = flat). The legacy SplatBake stays as the fallback and as the
  // ray-traced hit-shading approximation.
  struct SplatBakeV2 {
    asset::AssetId layers[8];
    asset::AssetId layer_normals[8];
    u32 layer_count = 0;
    asset::AssetId weights_a;  // palette slots 0-3
    asset::AssetId weights_b;  // palette slots 4-7
    bool ok = false;
  };
  SplatBakeV2 BakeSplatV2(const bethesda::Record& land, u16 land_plugin, i16 grid_x,
                          i16 grid_y);

  size_t baked_count() const { return baked_; }

 private:
  // An LTEX diffuse decoded to a small linear RGB float mip for sampling.
  struct Layer {
    u32 size = 0;
    base::Vector<f32> rgb;  // size * size * 3
  };

  const Layer* LayerFor(u64 ltex_packed);
  const Layer* DefaultLayer();
  bool DecodeTexture(const asset::Texture& texture, Layer* out) const;
  // Resolves an LTEX (0 = default) to its diffuse texture path / loaded asset.
  std::string LayerDiffusePath(u64 ltex_packed) const;
  asset::AssetId LayerAsset(u64 ltex_packed);
  // The LTEX texture set's normal map (TX01); zero id when the set has none.
  asset::AssetId LayerNormalAsset(u64 ltex_packed);
  // Resolves the bake/layer resolution from RX_LAND_BAKE_TEXELS on first use.
  void EnsureBakeSize();

  const bethesda::RecordStore& records_;
  asset::AssetDatabase& assets_;
  base::UnorderedMap<u64, Layer> layers_;  // LTEX id -> decoded diffuse
  Layer default_layer_;
  asset::AssetId default_albedo_;  // shared bake for layerless cells
  size_t baked_ = 0;
  u32 layer_size_ = 0;  // LTEX texels decoded per repeat (0 until resolved)
  u32 bake_size_ = 0;   // baked albedo texels across the whole cell
};

}  // namespace rx::world

#endif  // RECREATION_WORLD_LAND_BAKER_H_
