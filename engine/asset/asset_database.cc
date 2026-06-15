#include "asset/asset_database.h"

#include "core/log.h"

namespace rec::asset {
namespace {

base::String ExtensionOf(std::string_view normalized_path) {
  size_t dot = normalized_path.rfind('.');
  if (dot == std::string_view::npos) return {};
  std::string_view extension = normalized_path.substr(dot);
  return base::String(extension.data(), extension.size());
}

template <typename Asset, typename Converter>
const Asset* LoadWith(Vfs& vfs, std::string_view path,
                      const base::UnorderedMap<base::String, Converter>& converters,
                      base::UnorderedMap<u64, base::UniquePointer<Asset>>& cache) {
  std::string normalized = NormalizePath(path);
  AssetId id = MakeAssetId(normalized);
  if (auto* cached = cache.find(id.hash)) return cached->Get_UseOnlyIfYouKnowWhatYouareDoing();

  // Failures below cache a null entry so repeated lookups stay cheap.
  auto fail = [&](const char* what) -> const Asset* {
    REC_WARN("{}: {}", what, normalized);
    cache.emplace(id.hash, base::UniquePointer<Asset>());
    return nullptr;
  };

  const Converter* converter = converters.find(ExtensionOf(normalized));
  if (!converter) return fail("no converter for");
  auto bytes = vfs.Read(normalized);
  if (!bytes) return fail("asset not found");
  auto asset = (*converter)(ByteSpan(bytes->data(), bytes->size()), id, normalized);
  if (!asset) return fail("conversion failed");
  return cache.emplace(id.hash, std::move(asset))
      .first->Get_UseOnlyIfYouKnowWhatYouareDoing();
}

template <typename Asset>
const Asset* FindIn(const base::UnorderedMap<u64, base::UniquePointer<Asset>>& cache, AssetId id) {
  const auto* cached = cache.find(id.hash);
  return cached ? cached->Get_UseOnlyIfYouKnowWhatYouareDoing() : nullptr;
}

}  // namespace

void AssetDatabase::RegisterMeshConverter(base::String extension, MeshConverter converter) {
  mesh_converters_.emplace(extension, std::move(converter));
}

void AssetDatabase::RegisterTextureConverter(base::String extension, TextureConverter converter) {
  texture_converters_.emplace(extension, std::move(converter));
}

void AssetDatabase::RegisterMaterialConverter(base::String extension, MaterialConverter converter) {
  material_converters_.emplace(extension, std::move(converter));
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

void AssetDatabase::AddMaterial(const Material& material) {
  if (materials_.contains(material.id.hash)) return;
  materials_.emplace(material.id.hash, base::MakeUnique<Material>(material));
}

const Mesh* AssetDatabase::AddMesh(Mesh mesh) {
  u64 hash = mesh.id.hash;
  if (const auto* existing = meshes_.find(hash)) {
    return existing->Get_UseOnlyIfYouKnowWhatYouareDoing();
  }
  return meshes_.emplace(hash, base::MakeUnique<Mesh>(std::move(mesh)))
      .first->Get_UseOnlyIfYouKnowWhatYouareDoing();
}

const Material* AssetDatabase::FindMaterial(AssetId id) const {
  return FindIn(materials_, id);
}

const Texture* AssetDatabase::FindTexture(AssetId id) const { return FindIn(textures_, id); }

const Mesh* AssetDatabase::FindMesh(AssetId id) const { return FindIn(meshes_, id); }

}  // namespace rec::asset
