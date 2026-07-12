#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "core/log.h"

#include "asset/asset_database.h"
#include "asset/vfs.h"
#include "bethesda/archive.h"
#include "bethesda/converters.h"
#include "bethesda/game_profile.h"
#include "bethesda/nif.h"

// Loads one asset through the real Vfs + converter pipeline and dumps what
// came out. Handy for checking NIF/DDS conversion against game data. With an
// output path the raw vfs bytes are written instead, for external viewers.
int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: assetdump <data-dir> <virtual path> [out-file]\n");
    return 1;
  }
  using namespace rx;
  if (std::getenv("RX_LOG_DEBUG")) rx::SetLogLevel(rx::LogLevel::kDebug);

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
  if (path == "--list") {
    // Enumerate vfs entries containing the optional substring filter (argv[3]).
    std::string filter = argc > 3 ? argv[3] : "";
    u32 count = 0;
    vfs.Enumerate([&](std::string_view p) {
      if (filter.empty() || p.find(filter) != std::string_view::npos) {
        std::printf("%.*s\n", static_cast<int>(p.size()), p.data());
        ++count;
      }
    });
    std::printf("# %u entries\n", count);
    return 0;
  }
  if (path == "--nifscan") {
    // Convert every NIF in the vfs and report the failures: the coverage
    // oracle for the sequential (no block-size-table) readers.
    std::string filter = argc > 3 ? argv[3] : "";
    base::Vector<std::string> nifs;
    vfs.Enumerate([&](std::string_view p) {
      if (!p.ends_with(".nif")) return;
      if (!filter.empty() && p.find(filter) == std::string_view::npos) return;
      nifs.push_back(std::string(p));
    });
    u32 ok = 0, failed = 0, printed = 0;
    for (const std::string& nif : nifs) {
      auto bytes = vfs.Read(nif);
      if (!bytes) continue;
      bethesda::NifConversion conversion = bethesda::ConvertNifScene(
          rx::ByteSpan(bytes->data(), bytes->size()), asset::MakeAssetId(nif), nif);
      if (conversion.mesh && !conversion.mesh->lods.empty() &&
          !conversion.mesh->lods[0].vertices.empty()) {
        ++ok;
      } else {
        ++failed;
        if (printed < 1000) {
          std::printf("failed: %s\n", nif.c_str());
          ++printed;
        }
      }
    }
    std::printf("# nifscan: %u ok, %u failed of %zu\n", ok, failed,
                static_cast<size_t>(nifs.size()));
    return 0;
  }
  if (argc > 3) {
    auto bytes = vfs.Read(path);
    if (!bytes) {
      std::printf("not in vfs: %s\n", path.c_str());
      return 1;
    }
    std::FILE* out = std::fopen(argv[3], "wb");
    if (!out) return 1;
    std::fwrite(bytes->data(), 1, bytes->size(), out);
    std::fclose(out);
    std::printf("wrote %zu bytes to %s\n", static_cast<size_t>(bytes->size()), argv[3]);
    return 0;
  }
  if (path.ends_with(".nif") || path.ends_with(".btr") || path.ends_with(".bto") ||
      path.ends_with(".btt")) {
    // Convert directly (LoadMesh dispatches by .nif extension; .btr/.bto won't
    // route there, so use the conversion result for geometry).
    base::UnorderedMap<rx::u64, std::string> paths_by_id;
    auto bytes = vfs.Read(path);
    if (!bytes) {
      std::printf("not in vfs: %s\n", path.c_str());
      return 1;
    }
    bethesda::NifConversion conversion = bethesda::ConvertNifScene(
        rx::ByteSpan(bytes->data(), bytes->size()), asset::MakeAssetId(path), path);
    for (const std::string& texture : conversion.texture_paths) {
      paths_by_id.emplace(asset::MakeAssetId(texture).hash, texture);
    }
    if (conversion.skipped_shapes > 0) std::printf("skipped shapes: %u\n", conversion.skipped_shapes);
    auto path_of = [&](asset::AssetId id) -> const char* {
      const std::string* found = paths_by_id.find(id.hash);
      return found ? found->c_str() : "";
    };

    const asset::Mesh* mesh = conversion.mesh ? &*conversion.mesh : nullptr;
    if (!mesh || mesh->lods.empty()) {
      // The generic BSTriShape path came up empty; try the profile-registered
      // converter (Starfield BSGeometry NIFs only convert through it).
      mesh = database.LoadMesh(path);
    }
    if (!mesh || mesh->lods.empty()) {
      std::printf("mesh conversion failed\n");
      return 1;
    }
    const asset::MeshLod& lod = mesh->lods[0];
    std::printf("mesh: %zu vertices, %zu indices, %zu submeshes, bounds r=%.1f center=%.1f,%.1f,%.1f\n",
                lod.vertices.size(), lod.indices.size(), lod.submeshes.size(),
                mesh->bounds_radius, mesh->bounds_center[0], mesh->bounds_center[1],
                mesh->bounds_center[2]);
    if (!lod.vertices.empty()) {
      const asset::Vertex& v0 = lod.vertices.front();
      const asset::Vertex& vn = lod.vertices.back();
      std::printf("  v[0]=%.1f,%.1f,%.1f  v[last]=%.1f,%.1f,%.1f\n", v0.position[0], v0.position[1],
                  v0.position[2], vn.position[0], vn.position[1], vn.position[2]);
    }
    for (const asset::Submesh& submesh : lod.submeshes) {
      std::printf("  submesh +%u x%u material=%016llx\n", submesh.index_offset,
                  submesh.index_count,
                  static_cast<unsigned long long>(submesh.material.hash));
      const asset::Material* material = database.FindMaterial(submesh.material);
      if (!material) {
        std::printf("    MATERIAL MISSING\n");
        continue;
      }
      std::printf("    alpha_mode=%d cutoff=%.2f two_sided=%d rough=%.2f emissive=%.2f,%.2f,%.2f\n",
                  static_cast<int>(material->alpha_mode), material->alpha_cutoff,
                  material->two_sided, material->roughness_factor,
                  material->emissive_factor[0], material->emissive_factor[1],
                  material->emissive_factor[2]);
      std::printf("    base=%s normal=%s\n", path_of(material->base_color),
                  path_of(material->normal));
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
