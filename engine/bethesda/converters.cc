#include "bethesda/converters.h"

#include <algorithm>
#include <cstring>

#include "bethesda/nif.h"
#include "core/log.h"

namespace rec::bethesda {
namespace {

constexpr u32 kDdsMagic = FourCc('D', 'D', 'S', ' ');
constexpr u32 kDdpfFourCc = 0x4;
constexpr u32 kDdpfRgb = 0x40;
constexpr u32 kCaps2Cubemap = 0x200;

bool BlockCompressed(asset::TextureFormat format) {
  return format != asset::TextureFormat::kRgba8;
}

size_t MipChainSize(asset::TextureFormat format, u32 width, u32 height, u32 mips) {
  size_t total = 0;
  for (u32 m = 0; m < mips; ++m) {
    u32 w = std::max(1u, width >> m);
    u32 h = std::max(1u, height >> m);
    if (BlockCompressed(format)) {
      size_t block = (format == asset::TextureFormat::kBc1 ||
                      format == asset::TextureFormat::kBc4)
                         ? 8
                         : 16;
      total += ((w + 3) / 4) * ((h + 3) / 4) * block;
    } else {
      total += static_cast<size_t>(w) * h * 4;
    }
  }
  return total;
}

// Normal maps and other data textures must stay linear. Vanilla assets
// follow the _n / _msn suffix convention.
bool PathIsLinearData(std::string_view path) {
  size_t dot = path.rfind('.');
  std::string_view stem = dot == std::string_view::npos ? path : path.substr(0, dot);
  return stem.ends_with("_n") || stem.ends_with("_msn") || stem.ends_with("_normal");
}

base::UniquePointer<asset::Texture> ConvertDds(ByteSpan data, asset::AssetId id,
                                               std::string_view path) {
  if (data.size() < 128) return nullptr;
  u32 magic;
  std::memcpy(&magic, data.data(), 4);
  if (magic != kDdsMagic) return nullptr;

  auto texture = base::MakeUnique<asset::Texture>();
  texture->id = id;
  std::memcpy(&texture->height, data.data() + 12, 4);
  std::memcpy(&texture->width, data.data() + 16, 4);
  std::memcpy(&texture->mip_count, data.data() + 28, 4);
  if (texture->mip_count == 0) texture->mip_count = 1;

  u32 pf_flags, fourcc, caps2;
  std::memcpy(&pf_flags, data.data() + 80, 4);
  std::memcpy(&fourcc, data.data() + 84, 4);
  std::memcpy(&caps2, data.data() + 112, 4);
  if (caps2 & kCaps2Cubemap) return nullptr;  // environment maps, unused

  size_t data_offset = 128;
  bool srgb_from_format = false;
  bool linear_from_format = false;
  if ((pf_flags & kDdpfFourCc) && fourcc == FourCc('D', 'X', '1', '0')) {
    if (data.size() < 148) return nullptr;
    u32 dxgi;
    std::memcpy(&dxgi, data.data() + 128, 4);
    data_offset = 148;
    switch (dxgi) {
      case 71: texture->format = asset::TextureFormat::kBc1; linear_from_format = true; break;
      case 72: texture->format = asset::TextureFormat::kBc1; srgb_from_format = true; break;
      case 77: texture->format = asset::TextureFormat::kBc3; linear_from_format = true; break;
      case 78: texture->format = asset::TextureFormat::kBc3; srgb_from_format = true; break;
      case 80: texture->format = asset::TextureFormat::kBc4; break;
      case 83: texture->format = asset::TextureFormat::kBc5; break;
      case 98: texture->format = asset::TextureFormat::kBc7; linear_from_format = true; break;
      case 99: texture->format = asset::TextureFormat::kBc7; srgb_from_format = true; break;
      case 28: texture->format = asset::TextureFormat::kRgba8; linear_from_format = true; break;
      case 29: texture->format = asset::TextureFormat::kRgba8; srgb_from_format = true; break;
      default:
        REC_WARN("unsupported dxgi format {} in {}", dxgi, path);
        return nullptr;
    }
  } else if (pf_flags & kDdpfFourCc) {
    if (fourcc == FourCc('D', 'X', 'T', '1')) texture->format = asset::TextureFormat::kBc1;
    else if (fourcc == FourCc('D', 'X', 'T', '3')) texture->format = asset::TextureFormat::kBc3;
    else if (fourcc == FourCc('D', 'X', 'T', '5')) texture->format = asset::TextureFormat::kBc3;
    else if (fourcc == FourCc('A', 'T', 'I', '1') || fourcc == FourCc('B', 'C', '4', 'U'))
      texture->format = asset::TextureFormat::kBc4;
    else if (fourcc == FourCc('A', 'T', 'I', '2') || fourcc == FourCc('B', 'C', '5', 'U'))
      texture->format = asset::TextureFormat::kBc5;
    else {
      REC_WARN("unsupported dds fourcc in {}", path);
      return nullptr;
    }
  } else if (pf_flags & kDdpfRgb) {
    u32 bit_count, r_mask;
    std::memcpy(&bit_count, data.data() + 88, 4);
    std::memcpy(&r_mask, data.data() + 92, 4);
    if (bit_count != 32) {
      REC_WARN("unsupported uncompressed dds ({} bpp) in {}", bit_count, path);
      return nullptr;
    }
    texture->format = asset::TextureFormat::kRgba8;
    texture->data.assign(data.begin() + static_cast<std::ptrdiff_t>(data_offset), data.end());
    if (r_mask == 0x00ff0000) {  // BGRA on disk
      for (size_t i = 0; i + 3 < texture->data.size(); i += 4) {
        std::swap(texture->data[i], texture->data[i + 2]);
      }
    }
  } else {
    REC_WARN("unsupported dds pixel format in {}", path);
    return nullptr;
  }

  if (texture->data.empty()) {
    texture->data.assign(data.begin() + static_cast<std::ptrdiff_t>(data_offset), data.end());
  }

  // Trim the mip chain to what is actually present.
  while (texture->mip_count > 1 &&
         MipChainSize(texture->format, texture->width, texture->height, texture->mip_count) >
             texture->data.size()) {
    --texture->mip_count;
  }
  if (MipChainSize(texture->format, texture->width, texture->height, 1) > texture->data.size()) {
    REC_WARN("truncated dds: {}", path);
    return nullptr;
  }

  bool color = texture->format == asset::TextureFormat::kBc1 ||
               texture->format == asset::TextureFormat::kBc3 ||
               texture->format == asset::TextureFormat::kBc7 ||
               texture->format == asset::TextureFormat::kRgba8;
  if (srgb_from_format) texture->is_srgb = true;
  else if (linear_from_format) texture->is_srgb = false;
  else texture->is_srgb = color && !PathIsLinearData(path);
  if (!color) texture->is_srgb = false;
  return texture;
}

base::UniquePointer<asset::Material> ConvertBgsm(ByteSpan, asset::AssetId id, std::string_view) {
  auto material = base::MakeUnique<asset::Material>();
  material->id = id;
  // TODO: BGSM/BGEM layout (version, texture paths, alpha settings), then
  // map specular/smoothness to metallic/roughness.
  return material;
}

}  // namespace

void RegisterConverters(asset::AssetDatabase& database, const GameProfile& profile) {
  // The NIF converter synthesizes materials from the shader property blocks
  // and pre-loads their textures, so a converted mesh's submesh material ids
  // always resolve against the database.
  database.RegisterMeshConverter(
      ".nif", [&database](ByteSpan data, asset::AssetId id, std::string_view path) {
        NifConversion conversion = ConvertNifScene(data, id, path);
        if (!conversion.mesh) return base::UniquePointer<asset::Mesh>();
        for (const std::string& texture : conversion.texture_paths) {
          database.LoadTexture(texture);
        }
        for (const asset::Material& material : conversion.materials) {
          database.AddMaterial(material);
        }
        if (conversion.skipped_shapes > 0) {
          REC_DEBUG("{}: skipped {} shapes", path, conversion.skipped_shapes);
        }
        return std::move(conversion.mesh);
      });
  database.RegisterTextureConverter(".dds", ConvertDds);
  if (profile.game == Game::kFallout4 || profile.game == Game::kFallout76) {
    database.RegisterMaterialConverter(".bgsm", ConvertBgsm);
    database.RegisterMaterialConverter(".bgem", ConvertBgsm);
  }
  REC_INFO("registered bethesda converters for {}", profile.name);
}

}  // namespace rec::bethesda
