#ifndef RECREATION_ASSET_ASSET_DATABASE_H_
#define RECREATION_ASSET_ASSET_DATABASE_H_

#include <functional>
#include <memory>
#include <unordered_map>

#include "asset/asset_id.h"
#include "asset/material.h"
#include "asset/mesh.h"
#include "asset/texture.h"
#include "asset/vfs.h"

namespace rec::asset {

// Converts raw bytes from the Vfs into an engine asset. The bethesda module
// registers converters for .nif, .dds, .bgsm and friends. Keyed by extension
// so new formats plug in without touching this module.
using MeshConverter = std::function<std::unique_ptr<Mesh>(ByteSpan, AssetId)>;
using TextureConverter = std::function<std::unique_ptr<Texture>(ByteSpan, AssetId)>;
using MaterialConverter = std::function<std::unique_ptr<Material>(ByteSpan, AssetId)>;

class AssetDatabase {
 public:
  explicit AssetDatabase(Vfs& vfs) : vfs_(vfs) {}

  void RegisterMeshConverter(std::string extension, MeshConverter converter);
  void RegisterTextureConverter(std::string extension, TextureConverter converter);
  void RegisterMaterialConverter(std::string extension, MaterialConverter converter);

  // Loads (converting on first use) or returns the cached asset. Synchronous
  // for now, the streaming path will move conversion onto the job system.
  const Mesh* LoadMesh(std::string_view path);
  const Texture* LoadTexture(std::string_view path);
  const Material* LoadMaterial(std::string_view path);

  Vfs& vfs() { return vfs_; }

 private:
  Vfs& vfs_;
  std::unordered_map<std::string, MeshConverter> mesh_converters_;
  std::unordered_map<std::string, TextureConverter> texture_converters_;
  std::unordered_map<std::string, MaterialConverter> material_converters_;
  std::unordered_map<u64, std::unique_ptr<Mesh>> meshes_;
  std::unordered_map<u64, std::unique_ptr<Texture>> textures_;
  std::unordered_map<u64, std::unique_ptr<Material>> materials_;
};

}  // namespace rec::asset

#endif  // RECREATION_ASSET_ASSET_DATABASE_H_
