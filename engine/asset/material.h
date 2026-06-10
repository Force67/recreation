#ifndef RECREATION_ASSET_MATERIAL_H_
#define RECREATION_ASSET_MATERIAL_H_

#include "asset/asset_id.h"
#include "core/types.h"

namespace rec::asset {

enum class AlphaMode : u8 { kOpaque, kMask, kBlend };

// PBR metallic roughness. Legacy spec/gloss materials from the Bethesda
// shader sets are approximated into this during conversion.
struct Material {
  AssetId id;
  AssetId base_color;
  AssetId normal;
  AssetId metallic_roughness;
  AssetId emissive;
  f32 base_color_factor[4] = {1, 1, 1, 1};
  f32 metallic_factor = 0;
  f32 roughness_factor = 1;
  f32 emissive_factor[3] = {0, 0, 0};
  f32 alpha_cutoff = 0.5f;
  AlphaMode alpha_mode = AlphaMode::kOpaque;
  bool two_sided = false;
};

}  // namespace rec::asset

#endif  // RECREATION_ASSET_MATERIAL_H_
