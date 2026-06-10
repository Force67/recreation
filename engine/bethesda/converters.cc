#include "bethesda/converters.h"

#include <cstring>

#include "bethesda/nif.h"
#include "core/log.h"

namespace rec::bethesda {
namespace {

constexpr u32 kDdsMagic = FourCc('D', 'D', 'S', ' ');

std::unique_ptr<asset::Texture> ConvertDds(ByteSpan data, asset::AssetId id) {
  if (data.size() < 128) return nullptr;
  u32 magic;
  std::memcpy(&magic, data.data(), 4);
  if (magic != kDdsMagic) return nullptr;

  auto texture = std::make_unique<asset::Texture>();
  texture->id = id;
  std::memcpy(&texture->height, data.data() + 12, 4);
  std::memcpy(&texture->width, data.data() + 16, 4);
  std::memcpy(&texture->mip_count, data.data() + 28, 4);
  if (texture->mip_count == 0) texture->mip_count = 1;
  // TODO: pixel format from the DDS_PIXELFORMAT block and DX10 extension
  // header, srgb decision from usage (diffuse vs normal map).
  texture->data.assign(data.begin() + 128, data.end());
  return texture;
}

std::unique_ptr<asset::Material> ConvertBgsm(ByteSpan data, asset::AssetId id) {
  auto material = std::make_unique<asset::Material>();
  material->id = id;
  // TODO: BGSM/BGEM layout (version, texture paths, alpha settings), then
  // map specular/smoothness to metallic/roughness.
  return material;
}

}  // namespace

void RegisterConverters(asset::AssetDatabase& database, const GameProfile& profile) {
  database.RegisterMeshConverter(".nif", ConvertNif);
  database.RegisterTextureConverter(".dds", ConvertDds);
  if (profile.game == Game::kFallout4 || profile.game == Game::kFallout76) {
    database.RegisterMaterialConverter(".bgsm", ConvertBgsm);
    database.RegisterMaterialConverter(".bgem", ConvertBgsm);
  }
  REC_INFO("registered bethesda converters for {}", profile.name);
}

}  // namespace rec::bethesda
