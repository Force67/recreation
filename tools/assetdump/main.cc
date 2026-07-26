#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "core/log.h"

#include "anim/pose.h"
#include "asset/asset_database.h"
#include "asset/vfs.h"
#include "bethesda/archive.h"
#include "bethesda/converters.h"
#include "bethesda/game_profile.h"
#include "bethesda/kf_anim.h"
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
  if (path == "--kf") {
    // Convert a Gamebryo .kf clip against a skeleton and report the tracks, so
    // the B-spline decode can be checked without booting the engine.
    if (argc < 5) {
      std::printf("usage: assetdump <data-dir> --kf <clip.kf> <skeleton.nif>\n");
      return 1;
    }
    auto skel_bytes = vfs.Read(asset::NormalizePath(argv[4]));
    if (!skel_bytes) {
      std::printf("not in vfs: %s\n", argv[4]);
      return 1;
    }
    asset::Skeleton skeleton;
    if (!bethesda::ConvertNifSkeleton(rx::ByteSpan(skel_bytes->data(), skel_bytes->size()),
                                      asset::MakeAssetId(argv[4]), &skeleton)) {
      std::printf("skeleton parse failed\n");
      return 1;
    }
    auto bytes = vfs.Read(asset::NormalizePath(argv[3]));
    if (!bytes) {
      std::printf("not in vfs: %s\n", argv[3]);
      return 1;
    }
    asset::AnimationClip clip;
    if (!bethesda::ConvertKfAnimation(rx::ByteSpan(bytes->data(), bytes->size()),
                                      asset::MakeAssetId(argv[3]), skeleton, &clip)) {
      std::printf("kf conversion failed\n");
      return 1;
    }
    std::printf("clip %s: %.3fs loop=%d tracks=%zu (skeleton %zu bones)\n", argv[3], clip.duration,
                clip.loop, clip.tracks.size(), skeleton.bones.size());
    for (size_t i = 0; i < clip.tracks.size() && i < 12; ++i) {
      const asset::BoneTrack& t = clip.tracks[i];
      const char* name = t.bone >= 0 && t.bone < static_cast<i32>(skeleton.bones.size())
                             ? skeleton.bones[t.bone].name.c_str()
                             : "?";
      std::printf("  %-24s rot=%-4zu pos=%-4zu", name, t.rot.size(), t.pos.size());
      if (!t.rot.empty()) {
        const asset::RotKey& k = t.rot[t.rot.size() / 2];
        std::printf("  midq=(%.3f %.3f %.3f %.3f)", k.q[0], k.q[1], k.q[2], k.q[3]);
      }
      if (!t.pos.empty()) {
        const asset::PosKey& k = t.pos[t.pos.size() / 2];
        std::printf("  midp=(%.1f %.1f %.1f)", k.p[0], k.p[1], k.p[2]);
      }
      std::printf("\n");
    }
    return 0;
  }
  if (path == "--skin") {
    // Dump a skinned body part and, with a skeleton, the bind agreement between
    // the two: palette[i] = bone_model[remap[i]] * inverse_bind[i] must come out
    // near identity or the mesh renders exploded.
    if (argc < 4) {
      std::printf("usage: assetdump <data-dir> --skin <mesh.nif> [skeleton.nif]\n");
      return 1;
    }
    std::string mesh_path = argv[3];
    auto bytes = vfs.Read(asset::NormalizePath(mesh_path));
    if (!bytes) {
      std::printf("not in vfs: %s\n", mesh_path.c_str());
      return 1;
    }
    bethesda::NifConversion conv = bethesda::ConvertNifSkinnedMesh(
        rx::ByteSpan(bytes->data(), bytes->size()), asset::MakeAssetId(mesh_path), mesh_path);
    if (!conv.mesh || conv.mesh->lods.empty()) {
      std::printf("skinned conversion failed (skinned=%d)\n", conv.skinned);
      return 1;
    }
    const asset::MeshLod& lod = conv.mesh->lods[0];
    std::printf("skinned=%d bones=%zu vertices=%zu indices=%zu skinning=%zu submeshes=%zu\n",
                conv.skinned, conv.mesh->skin.bones.size(), lod.vertices.size(),
                lod.indices.size(), lod.skinning.size(), lod.submeshes.size());
    for (const asset::Submesh& s : lod.submeshes)
      std::printf("  submesh +%u x%u\n", s.index_offset, s.index_count);
    auto bbox = [](const base::Vector<asset::Vertex>& v, const char* label) {
      if (v.empty()) return;
      f32 mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
      for (const asset::Vertex& x : v)
        for (int k = 0; k < 3; ++k) {
          mn[k] = std::min(mn[k], x.position[k]);
          mx[k] = std::max(mx[k], x.position[k]);
        }
      std::printf("%s bbox (%.1f %.1f %.1f) - (%.1f %.1f %.1f)\n", label, mn[0], mn[1], mn[2], mx[0],
                  mx[1], mx[2]);
    };
    bbox(lod.vertices, "bind");

    if (argc < 5) {
      for (size_t i = 0; i < conv.mesh->skin.bones.size(); ++i) {
        const Mat4& ib = conv.mesh->skin.inverse_bind[i];
        std::printf("  bone[%2zu] %-28s inv_bind t=(%.1f %.1f %.1f)\n", i,
                    conv.mesh->skin.bones[i].c_str(), ib.m[12], ib.m[13], ib.m[14]);
      }
      return 0;
    }

    std::string skel_path = argv[4];
    auto skel_bytes = vfs.Read(asset::NormalizePath(skel_path));
    if (!skel_bytes) {
      std::printf("not in vfs: %s\n", skel_path.c_str());
      return 1;
    }
    asset::Skeleton skeleton;
    if (!bethesda::ConvertNifSkeleton(rx::ByteSpan(skel_bytes->data(), skel_bytes->size()),
                                      asset::MakeAssetId(skel_path), &skeleton)) {
      std::printf("skeleton parse failed\n");
      return 1;
    }
    anim::SkeletonPose pose;
    pose.ResetToBind(skeleton);
    base::Vector<Mat4> bone_model;
    anim::ComputeModelMatrices(skeleton, pose, &bone_model);
    base::Vector<i32> remap = anim::BuildBoneRemap(skeleton, conv.mesh->skin);
    std::printf("skeleton %s: %zu bones\n", skel_path.c_str(), skeleton.bones.size());

    base::Vector<Mat4> palette;
    anim::BuildSkinPalette(bone_model, conv.mesh->skin, remap, &palette);
    u32 missing = 0, off = 0;
    for (size_t i = 0; i < conv.mesh->skin.bones.size(); ++i) {
      if (remap[i] < 0) ++missing;
      // Deviation of palette[i] from identity: a correct bind pairing cancels.
      const Mat4& p = palette[i];
      f32 dev = 0;
      for (int k = 0; k < 16; ++k) dev = std::max(dev, std::abs(p.m[k] - Mat4::Identity().m[k]));
      if (dev > 0.5f) ++off;
      if (i < 40)
        std::printf("  bone[%2zu] %-28s remap=%3d dev=%7.2f palette_t=(%.1f %.1f %.1f)\n", i,
                    conv.mesh->skin.bones[i].c_str(), remap[i], dev, p.m[12], p.m[13], p.m[14]);
    }
    std::printf("bones: %zu total, %u unmatched, %u far from identity\n",
                conv.mesh->skin.bones.size(), missing, off);

    // Apply the palette exactly like the GPU does, so the printed bbox is what
    // the renderer would actually draw at bind pose.
    if (!lod.skinning.empty()) {
      base::Vector<asset::Vertex> posed = lod.vertices;
      for (size_t v = 0; v < posed.size() && v < lod.skinning.size(); ++v) {
        const asset::SkinnedVertexExtra& e = lod.skinning[v];
        f32 acc[3] = {0, 0, 0}, total = 0;
        for (int j = 0; j < 4; ++j) {
          f32 w = e.bone_weights[j] / 255.0f;
          if (w <= 0) continue;
          u32 b = e.bone_indices[j];
          if (b >= palette.size()) continue;
          const Mat4& m = palette[b];
          const f32* s = lod.vertices[v].position;
          for (int k = 0; k < 3; ++k)
            acc[k] += w * (m.m[k] * s[0] + m.m[4 + k] * s[1] + m.m[8 + k] * s[2] + m.m[12 + k]);
          total += w;
        }
        if (total > 1e-4f)
          for (int k = 0; k < 3; ++k) posed[v].position[k] = acc[k] / total;
      }
      bbox(posed, "posed");
    }
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
