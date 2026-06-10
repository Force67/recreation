#include "asset/asset_database.h"

#include "core/log.h"

namespace rec::asset {
namespace {

std::string ExtensionOf(std::string_view normalized_path) {
  size_t dot = normalized_path.rfind('.');
  if (dot == std::string_view::npos) return {};
  return std::string(normalized_path.substr(dot));
}

template <typename Asset, typename Converter>
const Asset* LoadWith(Vfs& vfs, std::string_view path,
                      const std::unordered_map<std::string, Converter>& converters,
                      std::unordered_map<u64, std::unique_ptr<Asset>>& cache) {
  std::string normalized = NormalizePath(path);
  AssetId id = MakeAssetId(normalized);
  if (auto it = cache.find(id.hash); it != cache.end()) return it->second.get();

  auto converter = converters.find(ExtensionOf(normalized));
  if (converter == converters.end()) {
    REC_WARN("no converter for {}", normalized);
    return nullptr;
  }
  auto bytes = vfs.Read(normalized);
  if (!bytes) {
    REC_WARN("asset not found: {}", normalized);
    return nullptr;
  }
  auto asset = converter->second(ByteSpan(*bytes), id);
  if (!asset) {
    REC_WARN("conversion failed: {}", normalized);
    return nullptr;
  }
  return cache.emplace(id.hash, std::move(asset)).first->second.get();
}

}  // namespace

void AssetDatabase::RegisterMeshConverter(std::string extension, MeshConverter converter) {
  mesh_converters_.emplace(std::move(extension), std::move(converter));
}

void AssetDatabase::RegisterTextureConverter(std::string extension, TextureConverter converter) {
  texture_converters_.emplace(std::move(extension), std::move(converter));
}

void AssetDatabase::RegisterMaterialConverter(std::string extension, MaterialConverter converter) {
  material_converters_.emplace(std::move(extension), std::move(converter));
}

const Mesh* AssetDatabase::LoadMesh(std::string_view path) {
  return LoadWith(vfs_, path, mesh_converters_, meshes_);
}

const Texture* AssetDatabase::LoadTexture(std::string_view path) {
  return LoadWith(vfs_, path, texture_converters_, textures_);
}

const Material* AssetDatabase::LoadMaterial(std::string_view path) {
  return LoadWith(vfs_, path, material_converters_, materials_);
}

}  // namespace rec::asset
