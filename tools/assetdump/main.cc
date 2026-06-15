#include <cstdio>
#include <filesystem>
#include <string>

#include "asset/asset_database.h"
#include "asset/vfs.h"
#include "bethesda/archive.h"
#include "bethesda/converters.h"
#include "bethesda/game_profile.h"

// Loads one asset through the real Vfs + converter pipeline and dumps what
// came out. Handy for checking NIF/DDS conversion against game data.
int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: assetdump <data-dir> <virtual path>\n");
    return 1;
  }
  using namespace rec;

  asset::Vfs vfs;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(argv[1], ec)) {
    if (auto provider = bethesda::OpenArchive(entry.path().string())) vfs.Mount(std::move(provider));
  }
  vfs.Mount(asset::MakeLooseFileProvider(argv[1]));

  asset::AssetDatabase database(vfs);
  const auto& profile = bethesda::GameProfile::For(
      bethesda::GameProfile::DetectFromDataDir(argv[1]));
  bethesda::RegisterConverters(database, profile);

  std::string path = argv[2];
  if (path.ends_with(".nif")) {
    const asset::Mesh* mesh = database.LoadMesh(path);
    if (!mesh) {
      std::printf("mesh conversion failed\n");
      return 1;
    }
    const asset::MeshLod& lod = mesh->lods[0];
    std::printf("mesh: %zu vertices, %zu indices, %zu submeshes, bounds r=%.1f\n",
                lod.vertices.size(), lod.indices.size(), lod.submeshes.size(),
                mesh->bounds_radius);
    for (const asset::Submesh& submesh : lod.submeshes) {
      std::printf("  submesh +%u x%u material=%016llx\n", submesh.index_offset,
                  submesh.index_count,
                  static_cast<unsigned long long>(submesh.material.hash));
      const asset::Material* material = database.FindMaterial(submesh.material);
      if (!material) {
        std::printf("    MATERIAL MISSING\n");
        continue;
      }
      std::printf("    alpha_mode=%d cutoff=%.2f two_sided=%d rough=%.2f base=%016llx "
                  "normal=%016llx\n",
                  static_cast<int>(material->alpha_mode), material->alpha_cutoff,
                  material->two_sided, material->roughness_factor,
                  static_cast<unsigned long long>(material->base_color.hash),
                  static_cast<unsigned long long>(material->normal.hash));
      for (asset::AssetId id : {material->base_color, material->normal}) {
        if (!id) continue;
        const asset::Texture* texture = database.FindTexture(id);
        if (!texture) {
          std::printf("    texture %016llx NOT LOADED\n",
                      static_cast<unsigned long long>(id.hash));
        } else {
          std::printf("    texture %016llx %ux%u mips=%u format=%d srgb=%d\n",
                      static_cast<unsigned long long>(id.hash), texture->width,
                      texture->height, texture->mip_count, static_cast<int>(texture->format),
                      texture->is_srgb);
        }
      }
    }
  } else {
    const asset::Texture* texture = database.LoadTexture(path);
    if (!texture) {
      std::printf("texture conversion failed\n");
      return 1;
    }
    std::printf("texture %ux%u mips=%u format=%d srgb=%d bytes=%zu\n", texture->width,
                texture->height, texture->mip_count, static_cast<int>(texture->format),
                texture->is_srgb, texture->data.size());
  }
  return 0;
}
