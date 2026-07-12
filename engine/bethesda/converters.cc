#include "bethesda/converters.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>

#include <base/containers/unordered_map.h>

#include "bethesda/material_db.h"
#include "bethesda/nif.h"
#include "bethesda/starfield_mesh.h"
#include "core/log.h"
#include "core/math.h"

namespace rx::bethesda {
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

}  // namespace

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
  if ((pf_flags & kDdpfFourCc) && fourcc == FourCc('D', 'X', '1', '0')) {
    if (data.size() < 148) return nullptr;
    u32 dxgi;
    std::memcpy(&dxgi, data.data() + 128, 4);
    data_offset = 148;
    // Bethesda authors color maps (albedo, glow) as the plain UNORM DXGI
    // formats, not the _SRGB variants, and relies on the texture's usage to
    // know they are sRGB. So a UNORM color-capable format is ambiguous: it may
    // be an sRGB albedo or a linear normal/data map stored in the same block
    // format. Only the explicit _SRGB variants are unambiguously sRGB; the
    // rest defer to the path heuristic below (matching the FourCC path).
    switch (dxgi) {
      case 71: texture->format = asset::TextureFormat::kBc1; break;
      case 72: texture->format = asset::TextureFormat::kBc1; srgb_from_format = true; break;
      case 74: texture->format = asset::TextureFormat::kBc2; break;
      case 75: texture->format = asset::TextureFormat::kBc2; srgb_from_format = true; break;
      case 77: texture->format = asset::TextureFormat::kBc3; break;
      case 78: texture->format = asset::TextureFormat::kBc3; srgb_from_format = true; break;
      case 80: texture->format = asset::TextureFormat::kBc4; break;
      case 83:  // BC5_UNORM
      case 84: texture->format = asset::TextureFormat::kBc5; break;  // BC5_SNORM (SF normals)
      case 87:  // B8G8R8A8_UNORM: swizzled to RGBA below
      case 28: texture->format = asset::TextureFormat::kRgba8; break;
      case 91:  // B8G8R8A8_UNORM_SRGB
      case 29: texture->format = asset::TextureFormat::kRgba8; srgb_from_format = true; break;
      case 98: texture->format = asset::TextureFormat::kBc7; break;
      case 99: texture->format = asset::TextureFormat::kBc7; srgb_from_format = true; break;
      default:
        RX_WARN("unsupported dxgi format {} in {}", dxgi, path);
        return nullptr;
    }
    // B8G8R8A8 ships blue and red swapped relative to the RGBA8 Vulkan format.
    if (dxgi == 87 || dxgi == 91) {
      texture->data.assign(data.begin() + static_cast<std::ptrdiff_t>(data_offset), data.end());
      for (size_t i = 0; i + 3 < texture->data.size(); i += 4) {
        std::swap(texture->data[i], texture->data[i + 2]);
      }
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
      RX_WARN("unsupported dds fourcc in {}", path);
      return nullptr;
    }
  } else if (pf_flags & kDdpfRgb) {
    u32 bit_count, r_mask;
    std::memcpy(&bit_count, data.data() + 88, 4);
    std::memcpy(&r_mask, data.data() + 92, 4);
    if (bit_count == 24) {
      // R8G8B8 / B8G8R8 (face tint masks ship as 24bpp uncompressed): expand to
      // rgba8 with opaque alpha. Widths are power-of-two so rows are 4-aligned
      // and the packed source tiles contiguously across the mip chain.
      texture->format = asset::TextureFormat::kRgba8;
      const u8* src = data.data() + data_offset;
      size_t texels = (data.size() - data_offset) / 3;
      bool bgr = r_mask == 0x00ff0000;
      texture->data.resize(texels * 4);
      for (size_t i = 0; i < texels; ++i) {
        u8 a = src[i * 3 + 0], b = src[i * 3 + 1], c = src[i * 3 + 2];
        texture->data[i * 4 + 0] = bgr ? c : a;
        texture->data[i * 4 + 1] = b;
        texture->data[i * 4 + 2] = bgr ? a : c;
        texture->data[i * 4 + 3] = 255;
      }
    } else if (bit_count == 32) {
      texture->format = asset::TextureFormat::kRgba8;
      texture->data.assign(data.begin() + static_cast<std::ptrdiff_t>(data_offset), data.end());
      if (r_mask == 0x00ff0000) {  // BGRA on disk
        for (size_t i = 0; i + 3 < texture->data.size(); i += 4) {
          std::swap(texture->data[i], texture->data[i + 2]);
        }
      }
    } else {
      RX_WARN("unsupported uncompressed dds ({} bpp) in {}", bit_count, path);
      return nullptr;
    }
  } else {
    RX_WARN("unsupported dds pixel format in {}", path);
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
    RX_WARN("truncated dds: {}", path);
    return nullptr;
  }

  bool color = texture->format == asset::TextureFormat::kBc1 ||
               texture->format == asset::TextureFormat::kBc2 ||
               texture->format == asset::TextureFormat::kBc3 ||
               texture->format == asset::TextureFormat::kBc7 ||
               texture->format == asset::TextureFormat::kRgba8;
  if (srgb_from_format) texture->is_srgb = true;
  else texture->is_srgb = color && !PathIsLinearData(path);
  if (!color) texture->is_srgb = false;
  return texture;
}

namespace {

std::string NormalizeTexturePathLocal(std::string_view raw) {
  if (raw.empty()) return {};
  std::string path = asset::NormalizePath(raw);
  size_t anchor = path.rfind("textures/");
  if (anchor != std::string::npos) return path.substr(anchor);
  return "textures/" + path;
}

// One length-prefixed, null-terminated string from a BGSM/BGEM at *cursor.
std::string ReadBgsmString(ByteSpan data, size_t* cursor) {
  if (*cursor + 4 > data.size()) return {};
  u32 length;
  std::memcpy(&length, data.data() + *cursor, 4);
  *cursor += 4;
  if (length > 1024 || *cursor + length > data.size()) return {};
  std::string s(reinterpret_cast<const char*>(data.data() + *cursor), length);
  *cursor += length;
  while (!s.empty() && s.back() == '\0') s.pop_back();
  return s;
}

// Fallout 4 BGSM (lighting) / BGEM (effect) material file, version 2. The
// pre-texture header is a fixed size, so the texture string block starts at a
// constant offset. Slot 0 is the base/diffuse texture; for BGSM slot 1 is the
// normal map (BGEM slot 1 is a greyscale mask, not a normal, so it is ignored).
constexpr size_t kBgsmV2TextureOffset = 63;

// BGSM lays ten texture slots, then a fixed block (flags, a scalar, the
// specular colour and strength), then the smoothness float. Verified against
// shipped FO4 materials: the smoothness sits this far past the slots and reads
// in [0,1] for ~99% of them (the rest are clamped).
constexpr size_t kBgsmV2TextureSlots = 10;
constexpr size_t kBgsmV2SmoothnessGap = 28;

base::UniquePointer<asset::Material> ConvertBgsm(ByteSpan data, asset::AssetId id,
                                                 std::string_view) {
  auto material = base::MakeUnique<asset::Material>();
  material->id = id;
  BgsmMaterial parsed;
  if (ParseBgsm(data, &parsed)) {
    if (!parsed.diffuse.empty()) material->base_color = asset::MakeAssetId(parsed.diffuse);
    if (!parsed.normal.empty()) material->normal = asset::MakeAssetId(parsed.normal);
    if (parsed.roughness >= 0.0f) material->roughness_factor = parsed.roughness;
  }
  return material;
}

}  // namespace

// roughness stays negative (unknown) for BGEM or a short read, so the caller
// keeps its own default; for BGSM it is 1 - smoothness.
bool ParseBgsm(ByteSpan data, BgsmMaterial* out) {
  *out = BgsmMaterial{};
  if (data.size() < kBgsmV2TextureOffset + 8) return false;
  const bool bgsm = std::memcmp(data.data(), "BGSM", 4) == 0;
  const bool bgem = std::memcmp(data.data(), "BGEM", 4) == 0;
  if (!bgsm && !bgem) return false;
  u32 version;
  std::memcpy(&version, data.data() + 4, 4);
  if (version != 2) return false;  // FO76 (v20+) has a different header

  size_t cursor = kBgsmV2TextureOffset;
  std::string base = ReadBgsmString(data, &cursor);
  std::string second = ReadBgsmString(data, &cursor);
  out->diffuse = NormalizeTexturePathLocal(base);
  out->normal = bgsm ? NormalizeTexturePathLocal(second) : std::string();

  if (bgsm) {
    for (size_t i = 2; i < kBgsmV2TextureSlots; ++i) ReadBgsmString(data, &cursor);
    cursor += kBgsmV2SmoothnessGap;
    if (cursor + 4 <= data.size()) {
      f32 smoothness;
      std::memcpy(&smoothness, data.data() + cursor, 4);
      out->roughness = 1.0f - std::clamp(smoothness, 0.0f, 1.0f);
    }
  }
  return !out->diffuse.empty();
}

namespace {

// The one ".mat" path a Starfield NIF references, or empty when it carries none
// or several (an unambiguous single material is the case the convention below
// can texture safely). Material paths live in the NIF header string table.
std::string SingleMaterialPath(ByteSpan nif) {
  auto header = ParseNifHeader(nif);
  if (!header) return {};
  std::string found;
  for (const std::string& s : header->strings) {
    std::string norm = asset::NormalizePath(s);
    if (!norm.ends_with(".mat") || norm.find("materials/") == std::string::npos) continue;
    if (found.empty()) {
      found = std::move(norm);
    } else if (found != norm) {
      return {};  // more than one distinct material: ambiguous, leave untextured
    }
  }
  return found;
}

// Binds `path` to a material texture slot when the file exists, so a mesh is
// never given a missing or wrong texture.
void BindTextureIfExists(asset::AssetDatabase& database, const std::string& path,
                         asset::AssetId* slot) {
  if (path.empty() || !database.vfs().Contains(path)) return;
  *slot = asset::MakeAssetId(path);
  database.LoadTexture(path);
}

// Emissive, unlike the other maps, needs its factor turned on to show.
void BindEmissiveIfExists(asset::AssetDatabase& database, const std::string& path,
                          asset::Material* material) {
  if (path.empty() || !database.vfs().Contains(path)) return;
  material->emissive = asset::MakeAssetId(path);
  for (int k = 0; k < 3; ++k) material->emissive_factor[k] = 1.0f;
  database.LoadTexture(path);
}

// Binds the base-color, normal and emissive textures a ".mat" implies by
// Starfield's path mirror convention (Materials\X\Y.mat -> textures/X/Y_color.dds),
// but only when the files actually exist, so a mesh is never bound a wrong
// texture. The convention holds for landscape and natural materials; the
// material database resolves the rest.
void BindConventionTextures(asset::AssetDatabase& database, const std::string& mat_path,
                            asset::Material* material) {
  size_t anchor = mat_path.find("materials/");
  if (anchor == std::string::npos || mat_path.size() < anchor + 14) return;
  std::string stem = mat_path.substr(anchor + 10, mat_path.size() - anchor - 10 - 4);  // drop ".mat"
  BindTextureIfExists(database, "textures/" + stem + "_color.dds", &material->base_color);
  BindTextureIfExists(database, "textures/" + stem + "_normal.dds", &material->normal);
  BindEmissiveIfExists(database, "textures/" + stem + "_emissive.dds", material);
}

// Resolves a Starfield mesh's textures: the compiled material database first (it
// maps most materials, including architecture and ships), then the path
// convention (landscape), then nothing (the gray factor shows through).
void BindStarfieldMaterial(asset::AssetDatabase& database, const StarfieldMaterialDb& mat_db,
                           const std::string& mat_path, asset::Material* material) {
  std::string color, normal, emissive;
  if (mat_db.Lookup(mat_path, &color, &normal, &emissive)) {
    BindTextureIfExists(database, color, &material->base_color);
    BindTextureIfExists(database, normal, &material->normal);
    BindEmissiveIfExists(database, emissive, material);
    return;
  }
  BindConventionTextures(database, mat_path, material);
}

// A Starfield body NIF carries its skin binding in three blocks: SkinAttach
// (the ordered list of skin bone names, length-prefixed), BSSkin::BoneData (the
// per-bone inverse-bind transform), and BSSkin::Instance (refs, mostly -1 since
// the bones resolve by name against an external skeleton). The weight stream's
// boneIndex indexes this list. Fills `skin` from SkinAttach + BoneData; returns
// false when either is absent or their bone counts disagree. The inverse-bind
// translations stay in metres, matching the metres-space skeleton.
bool ParseStarfieldSkin(ByteSpan data, asset::SkinBinding* skin) {
  auto header = ParseNifHeader(data);
  if (!header) return false;
  u32 block_count = static_cast<u32>(header->block_sizes.size());

  base::Vector<std::string> names;
  base::Vector<Mat4> inverse_bind;
  for (u32 i = 0; i < block_count; ++i) {
    const std::string& type = header->block_types[header->block_type_index[i]];
    const u8* block = data.data() + header->block_offsets[i];
    size_t size = header->block_sizes[i];
    if (type == "SkinAttach") {
      // u32 (ref, unused), u32 bone count, then `count` length-prefixed strings.
      if (size < 8) continue;
      u32 count;
      std::memcpy(&count, block + 4, 4);
      if (count > 256) continue;
      size_t cursor = 8;
      bool ok = true;
      for (u32 b = 0; b < count && ok; ++b) {
        if (cursor + 4 > size) { ok = false; break; }
        u32 len;
        std::memcpy(&len, block + cursor, 4);
        cursor += 4;
        if (len > 256 || cursor + len > size) { ok = false; break; }
        names.emplace_back(reinterpret_cast<const char*>(block + cursor), len);
        cursor += len;
      }
      if (!ok) names.clear();
    } else if (type == "BSSkin::BoneData") {
      // u32 bone count, then per bone: NiBound (center vec3 + radius, 16 bytes)
      // followed by NiTransform (9 f32 row-major rotation, vec3 translation, f32
      // scale, 52 bytes). The transform is the inverse bind (bind vertex -> bone
      // local) the GPU palette wants.
      if (size < 4) continue;
      u32 count;
      std::memcpy(&count, block, 4);
      constexpr size_t kBoneStride = 16 + 52;
      if (count > 256 || 4 + static_cast<size_t>(count) * kBoneStride > size) continue;
      inverse_bind.reserve(count);
      for (u32 b = 0; b < count; ++b) {
        const u8* t = block + 4 + static_cast<size_t>(b) * kBoneStride + 16;  // skip NiBound
        f32 rot[9], trans[3], scale;
        std::memcpy(rot, t, 36);
        std::memcpy(trans, t + 36, 12);
        std::memcpy(&scale, t + 48, 4);
        // Row-major rotation (r[row*3+col]) and scale into the column-major Mat4
        // (m[col*4+row]), then the translation, matching the engine's MakeTransform.
        Mat4 m = Mat4::Identity();
        for (int row = 0; row < 3; ++row) {
          for (int col = 0; col < 3; ++col) m.m[col * 4 + row] = rot[row * 3 + col] * scale;
        }
        m.m[12] = trans[0];
        m.m[13] = trans[1];
        m.m[14] = trans[2];
        inverse_bind.push_back(m);
      }
    }
  }

  if (names.empty() || names.size() != inverse_bind.size()) return false;
  skin->bones = std::move(names);
  skin->inverse_bind = std::move(inverse_bind);
  return true;
}

// Starfield NIFs reference external ".mesh" geometry by hash rather than
// inlining vertices, so its converter loads each referenced mesh from the vfs,
// bakes the node transform into the vertices, and binds each geometry's own
// ".mat" (from its shader property) through the material database. Distinct
// materials in one NIF stay distinct per submesh. Returns null when nothing
// converts.
base::UniquePointer<asset::Mesh> ConvertStarfieldNif(asset::AssetDatabase& database,
                                                     const StarfieldMaterialDb& mat_db,
                                                     ByteSpan data, asset::AssetId id,
                                                     std::string_view path) {
  base::Vector<StarfieldGeometryRef> refs;
  if (!ParseStarfieldNif(data, &refs)) return nullptr;

  auto mesh = base::MakeUnique<asset::Mesh>();
  mesh->id = id;
  mesh->lods.emplace_back();
  asset::MeshLod& lod = mesh->lods[0];

  // One asset::Material per distinct .mat path in this NIF (many geometries
  // share the same material), keyed by the material path hash so a submesh binds
  // its own textures. Geometries without a resolved .mat share a mid-gray
  // default (key 0).
  base::UnorderedMap<u64, asset::AssetId> materials_by_path;
  auto material_for = [&](const std::string& mat_path) -> asset::AssetId {
    u64 key = mat_path.empty() ? 0 : asset::MakeAssetId(mat_path).hash;
    if (const asset::AssetId* found = materials_by_path.find(key)) return *found;
    asset::Material material;
    material.id = asset::MakeAssetId(std::string(path) + "#m" +
                                     std::to_string(materials_by_path.size()));
    for (int k = 0; k < 3; ++k) material.base_color_factor[k] = 0.5f;
    material.roughness_factor = 0.8f;
    material.metallic_factor = 0;
    if (!mat_path.empty()) BindStarfieldMaterial(database, mat_db, mat_path, &material);
    database.AddMaterial(material);
    materials_by_path.emplace(key, material.id);
    return material.id;
  };

  f32 bounds_min[3] = {1e30f, 1e30f, 1e30f};
  f32 bounds_max[3] = {-1e30f, -1e30f, -1e30f};
  for (const StarfieldGeometryRef& ref : refs) {
    auto bytes = database.vfs().Read(ref.mesh_path);
    if (!bytes) continue;
    StarfieldMeshData geometry;
    if (!ParseStarfieldMesh(ByteSpan(bytes->data(), bytes->size()), &geometry)) continue;

    u32 vertex_base = static_cast<u32>(lod.vertices.size());
    u32 index_offset = static_cast<u32>(lod.indices.size());
    for (const asset::Vertex& src : geometry.vertices) {
      asset::Vertex v = src;
      // Bake the node-chain transform: p' = rotation * p * scale + translation.
      // The NIF authors translations in metres while the parsed vertices were
      // lifted to game units, so the translation lifts by the same factor
      // (unscaled it collapses multi-part NIFs 70x toward their origin).
      for (int i = 0; i < 3; ++i) {
        v.position[i] = (ref.rotation[i * 3] * src.position[0] +
                         ref.rotation[i * 3 + 1] * src.position[1] +
                         ref.rotation[i * 3 + 2] * src.position[2]) *
                            ref.scale +
                        ref.translation[i] * kStarfieldMetresToGameUnits;
        v.normal[i] = ref.rotation[i * 3] * src.normal[0] +
                      ref.rotation[i * 3 + 1] * src.normal[1] +
                      ref.rotation[i * 3 + 2] * src.normal[2];
      }
      for (int k = 0; k < 3; ++k) {
        bounds_min[k] = std::min(bounds_min[k], v.position[k]);
        bounds_max[k] = std::max(bounds_max[k], v.position[k]);
      }
      lod.vertices.push_back(v);
    }
    for (u32 index : geometry.indices) lod.indices.push_back(vertex_base + index);

    asset::Submesh submesh;
    submesh.index_offset = index_offset;
    submesh.index_count = static_cast<u32>(geometry.indices.size());
    submesh.material = material_for(ref.material_path);
    lod.submeshes.push_back(submesh);
  }

  if (lod.vertices.empty()) return nullptr;
  for (int k = 0; k < 3; ++k) mesh->bounds_center[k] = (bounds_min[k] + bounds_max[k]) * 0.5f;
  f32 radius_sq = 0;
  for (int k = 0; k < 3; ++k) {
    f32 d = bounds_max[k] - mesh->bounds_center[k];
    radius_sq += d * d;
  }
  mesh->bounds_radius = std::sqrt(radius_sq);
  return mesh;
}

}  // namespace

base::UniquePointer<asset::Mesh> ConvertStarfieldSkinnedNif(asset::AssetDatabase& database,
                                                            const StarfieldMaterialDb& mat_db,
                                                            ByteSpan data, asset::AssetId id,
                                                            std::string_view path) {
  asset::SkinBinding skin;
  if (!ParseStarfieldSkin(data, &skin)) return nullptr;
  base::Vector<StarfieldGeometryRef> refs;
  if (!ParseStarfieldNif(data, &refs)) return nullptr;

  auto mesh = base::MakeUnique<asset::Mesh>();
  mesh->id = id;
  mesh->lods.emplace_back();
  asset::MeshLod& lod = mesh->lods[0];

  asset::Material material;
  material.id = asset::MakeAssetId(std::string(path) + "#m0");
  for (int k = 0; k < 3; ++k) material.base_color_factor[k] = 0.5f;
  material.roughness_factor = 0.8f;
  material.metallic_factor = 0;
  if (std::string mat_path = SingleMaterialPath(data); !mat_path.empty()) {
    BindStarfieldMaterial(database, mat_db, mat_path, &material);
  }
  database.AddMaterial(material);

  f32 bounds_min[3] = {1e30f, 1e30f, 1e30f};
  f32 bounds_max[3] = {-1e30f, -1e30f, -1e30f};
  for (const StarfieldGeometryRef& ref : refs) {
    auto bytes = database.vfs().Read(ref.mesh_path);
    if (!bytes) continue;
    StarfieldSkinnedMeshData geometry;
    if (!ParseStarfieldSkinnedMesh(ByteSpan(bytes->data(), bytes->size()), &geometry)) continue;

    // Skinned vertices stay in bind space: the node transform is the skeleton's
    // job, so unlike ConvertStarfieldNif it is not baked into the vertices here.
    u32 vertex_base = static_cast<u32>(lod.vertices.size());
    u32 index_offset = static_cast<u32>(lod.indices.size());
    for (size_t vi = 0; vi < geometry.vertices.size(); ++vi) {
      const asset::Vertex& v = geometry.vertices[vi];
      for (int k = 0; k < 3; ++k) {
        bounds_min[k] = std::min(bounds_min[k], v.position[k]);
        bounds_max[k] = std::max(bounds_max[k], v.position[k]);
      }
      lod.vertices.push_back(v);
      lod.skinning.push_back(geometry.skinning[vi]);
    }
    for (u32 index : geometry.indices) lod.indices.push_back(vertex_base + index);

    asset::Submesh submesh;
    submesh.index_offset = index_offset;
    submesh.index_count = static_cast<u32>(geometry.indices.size());
    submesh.material = material.id;
    lod.submeshes.push_back(submesh);
  }

  if (lod.vertices.empty()) return nullptr;
  mesh->skinned = true;
  mesh->skin = std::move(skin);
  for (int k = 0; k < 3; ++k) mesh->bounds_center[k] = (bounds_min[k] + bounds_max[k]) * 0.5f;
  f32 radius_sq = 0;
  for (int k = 0; k < 3; ++k) {
    f32 d = bounds_max[k] - mesh->bounds_center[k];
    radius_sq += d * d;
  }
  mesh->bounds_radius = std::sqrt(radius_sq);
  return mesh;
}

void RegisterConverters(asset::AssetDatabase& database, const GameProfile& profile) {
  if (profile.game == Game::kStarfield) {
    // Parse the compiled material database once: the 105MB blob is read, the
    // material->texture index extracted, then the blob dropped. The converter
    // lambda holds the small index by shared pointer.
    auto material_db = std::make_shared<StarfieldMaterialDb>();
    if (auto cdb = database.vfs().Read("materials/materialsbeta.cdb")) {
      material_db->Build(ByteSpan(cdb->data(), cdb->size()));
      RX_INFO("starfield material database: {} materials indexed ({} via object graph)",
              material_db->size(), material_db->graph_size());
    } else {
      RX_WARN("starfield material database not found; meshes use the path convention only");
    }
    database.RegisterMeshConverter(
        ".nif",
        [&database, material_db](ByteSpan data, asset::AssetId id, std::string_view path) {
          return ConvertStarfieldNif(database, *material_db, data, id, path);
        });
    database.RegisterTextureConverter(".dds", ConvertDds);
    RX_INFO("registered bethesda converters for {}", profile.name);
    return;
  }
  // The NIF converter synthesizes materials from the shader property blocks
  // and pre-loads their textures, so a converted mesh's submesh material ids
  // always resolve against the database. Distant LOD meshes (.btr terrain,
  // .bto objects, .btt trees) are the same NIF format, so they route here too.
  asset::MeshConverter nif_converter =
      [&database](ByteSpan data, asset::AssetId id, std::string_view path) {
        NifConversion conversion = ConvertNifScene(data, id, path);
        if (!conversion.mesh) return base::UniquePointer<asset::Mesh>();
        for (const std::string& texture : conversion.texture_paths) {
          // Gamebryo (Oblivion) normal maps are derived by the _n.dds naming
          // convention, so only bind the ones that actually exist.
          if (conversion.gamebryo && texture.ends_with("_n.dds") &&
              !database.vfs().Contains(texture)) {
            asset::AssetId missing = asset::MakeAssetId(texture);
            for (asset::Material& material : conversion.materials) {
              if (material.normal.hash == missing.hash) material.normal = {};
            }
            continue;
          }
          database.LoadTexture(texture);
        }
        // Fallout 4 binds textures through a .bgsm/.bgem material file; the
        // material file is authoritative, so it overrides the inline texture
        // set the lighting shader may also carry (FO4 NIFs ship both, and the
        // inline slots are unreliable -- e.g. architecture meshes bind a normal
        // map there, which would render as a purple albedo).
        for (size_t i = 0; i < conversion.materials.size(); ++i) {
          asset::Material& material = conversion.materials[i];
          const std::string& material_file =
              i < conversion.material_files.size() ? conversion.material_files[i] : std::string();
          if (material_file.empty()) continue;
          auto bytes = database.vfs().Read(material_file);
          if (!bytes) continue;
          BgsmMaterial parsed;
          if (!ParseBgsm(ByteSpan(bytes->data(), bytes->size()), &parsed)) continue;
          if (!parsed.diffuse.empty()) {
            material.base_color = asset::MakeAssetId(parsed.diffuse);
            database.LoadTexture(parsed.diffuse);
          }
          if (!parsed.normal.empty()) {
            material.normal = asset::MakeAssetId(parsed.normal);
            database.LoadTexture(parsed.normal);
          }
          if (parsed.roughness >= 0.0f) material.roughness_factor = parsed.roughness;
        }
        for (const asset::Material& material : conversion.materials) {
          database.AddMaterial(material);
        }
        if (conversion.skipped_shapes > 0) {
          RX_DEBUG("{}: skipped {} shapes", path, conversion.skipped_shapes);
        }
        if (conversion.refraction_shapes > 0) {
          RX_INFO("{}: {} refraction shapes routed to transmission", path,
                   conversion.refraction_shapes);
        }
        if (conversion.effect_shapes > 0) {
          RX_INFO("vfx: {} effect-shader shapes {}", conversion.effect_shapes, path);
        }
        if (!conversion.mesh->emitters.empty()) {
          RX_INFO("vfx: {} particle emitters {}", conversion.mesh->emitters.size(), path);
        }
        // Distant LOD proxies (.btr/.bto/.btt) must stay out of the tlas: they
        // would double the geometry the full-detail near meshes already provide
        // to ray queries (shadows, ao, reflections).
        if (path.ends_with(".btr") || path.ends_with(".bto") || path.ends_with(".btt")) {
          conversion.mesh->exclude_from_rt = true;
          conversion.mesh->terrain_lod = path.ends_with(".btr");
        }
        return std::move(conversion.mesh);
      };
  database.RegisterMeshConverter(".nif", nif_converter);
  database.RegisterMeshConverter(".btr", nif_converter);  // terrain LOD (quad-local verts)
  database.RegisterMeshConverter(".bto", nif_converter);  // object LOD (absolute world verts)
  database.RegisterMeshConverter(".btt", nif_converter);  // tree LOD
  database.RegisterTextureConverter(".dds", ConvertDds);
  if (profile.game == Game::kFallout4 || profile.game == Game::kFallout76) {
    database.RegisterMaterialConverter(".bgsm", ConvertBgsm);
    database.RegisterMaterialConverter(".bgem", ConvertBgsm);
  }
  RX_INFO("registered bethesda converters for {}", profile.name);
}

}  // namespace rx::bethesda
