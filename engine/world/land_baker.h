#ifndef RECREATION_WORLD_LAND_BAKER_H_
#define RECREATION_WORLD_LAND_BAKER_H_

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "asset/asset_database.h"
#include "bethesda/load_order.h"
#include "bethesda/record.h"
#include "core/types.h"

namespace rec::world {

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

  const bethesda::RecordStore& records_;
  asset::AssetDatabase& assets_;
  base::UnorderedMap<u64, Layer> layers_;  // LTEX id -> decoded diffuse
  Layer default_layer_;
  asset::AssetId default_albedo_;  // shared bake for layerless cells
  size_t baked_ = 0;
};

}  // namespace rec::world

#endif  // RECREATION_WORLD_LAND_BAKER_H_
