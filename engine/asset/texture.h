#ifndef RECREATION_ASSET_TEXTURE_H_
#define RECREATION_ASSET_TEXTURE_H_

#include <vector>

#include "asset/asset_id.h"
#include "core/types.h"

namespace rec::asset {

enum class TextureFormat : u8 {
  kUnknown,
  kRgba8,
  kBc1,
  kBc3,
  kBc4,
  kBc5,
  kBc7,
};

// Source DDS data is mostly BCn already and uploads as is. Only legacy
// uncompressed or palettized textures need conversion.
struct Texture {
  AssetId id;
  TextureFormat format = TextureFormat::kUnknown;
  u32 width = 0;
  u32 height = 0;
  u32 mip_count = 1;
  u32 array_layers = 1;
  bool is_cubemap = false;
  bool is_srgb = false;
  std::vector<u8> data;
};

}  // namespace rec::asset

#endif  // RECREATION_ASSET_TEXTURE_H_
