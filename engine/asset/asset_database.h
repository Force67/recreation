#ifndef RECREATION_ASSET_ASSET_DATABASE_H_
#define RECREATION_ASSET_ASSET_DATABASE_H_

#include <functional>

#include <base/containers/unordered_map.h>
#include <base/memory/unique_pointer.h>
#include <base/strings/xstring.h>

#include "asset/asset_id.h"
#include "asset/material.h"
#include "asset/mesh.h"
#include "asset/texture.h"
#include "asset/vfs.h"

namespace rec::asset {

// Converts raw bytes from the Vfs into an engine asset. The bethesda module
// registers converters for .nif, .dds, .bgsm and friends. Keyed by extension
// so new formats plug in without touching this module. The normalized source
// path rides along for converters that key behavior off naming conventions
// (e.g. _n.dds normal maps stay linear).
using MeshConverter =
    std::function<base::UniquePointer<Mesh>(ByteSpan, AssetId, std::string_view path)>;
using TextureConverter =
    std::function<base::UniquePointer<Texture>(ByteSpan, AssetId, std::string_view path)>;
using MaterialConverter =
    std::function<base::UniquePointer<Material>(ByteSpan, AssetId, std::string_view path)>;

class AssetDatabase {
 public:
  explicit AssetDatabase(Vfs& vfs) : vfs_(vfs) {}

  void RegisterMeshConverter(base::String extension, MeshConverter converter);
  void RegisterTextureConverter(base::String extension, TextureConverter converter);
  void RegisterMaterialConverter(base::String extension, MaterialConverter converter);

  // Loads (converting on first use) or returns the cached asset. Synchronous
  // for now, the streaming path will move conversion onto the job system.
  // Failures cache as null so missing files are only probed once.
  const Mesh* LoadMesh(std::string_view path);
  const Texture* LoadTexture(std::string_view path);
  const Material* LoadMaterial(std::string_view path);

  // Side channel for converters that synthesize assets while converting
  // another (NIF shader properties become materials) and for procedurally
  // built meshes (terrain). Keyed by their id.
  void AddMaterial(const Material& material);
  const Mesh* AddMesh(Mesh mesh);
  const Material* FindMaterial(AssetId id) const;
  const Texture* FindTexture(AssetId id) const;
  const Mesh* FindMesh(AssetId id) const;

  Vfs& vfs() { return vfs_; }

 private:
  Vfs& vfs_;
  base::UnorderedMap<base::String, MeshConverter> mesh_converters_;
  base::UnorderedMap<base::String, TextureConverter> texture_converters_;
  base::UnorderedMap<base::String, MaterialConverter> material_converters_;
  base::UnorderedMap<u64, base::UniquePointer<Mesh>> meshes_;
  base::UnorderedMap<u64, base::UniquePointer<Texture>> textures_;
  base::UnorderedMap<u64, base::UniquePointer<Material>> materials_;
};

}  // namespace rec::asset

#endif  // RECREATION_ASSET_ASSET_DATABASE_H_
