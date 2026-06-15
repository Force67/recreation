#include "bethesda/nif.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <base/containers/unordered_map.h>

#include "asset/asset_id.h"
#include "core/log.h"

namespace rec::bethesda {
namespace {

constexpr std::string_view kMagic = "Gamebryo File Format, Version ";
constexpr u32 kVersion20_2_0_7 = 0x14020007;

struct Reader {
  ByteSpan data;
  size_t pos = 0;
  bool ok = true;

  template <typename T>
  T Read() {
    T value{};
    if (pos + sizeof(T) > data.size()) {
      ok = false;
      return value;
    }
    std::memcpy(&value, data.data() + pos, sizeof(T));
    pos += sizeof(T);
    return value;
  }

  void Skip(size_t count) {
    if (pos + count > data.size()) {
      ok = false;
      return;
    }
    pos += count;
  }

  const u8* Bytes(size_t count) {
    if (pos + count > data.size()) {
      ok = false;
      return nullptr;
    }
    const u8* p = data.data() + pos;
    pos += count;
    return p;
  }
};

f32 HalfToFloat(u16 h) {
  u32 sign = static_cast<u32>(h >> 15) & 1;
  u32 exponent = (h >> 10) & 0x1f;
  u32 mantissa = h & 0x3ff;
  u32 bits;
  if (exponent == 0) {
    // Subnormals flush to zero, irrelevant at mesh scale.
    bits = sign << 31;
  } else if (exponent == 31) {
    return 0;  // inf/nan in source data, neutralize
  } else {
    bits = sign << 31 | (exponent + 112) << 23 | mantissa << 13;
  }
  f32 out;
  std::memcpy(&out, &bits, 4);
  return out;
}

f32 ByteToSnorm(u8 b) { return static_cast<f32>(b) / 255.0f * 2.0f - 1.0f; }

// p' = rotation * p * scale + translation, rotation rows stored in file order.
struct Transform {
  f32 r[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  f32 t[3] = {0, 0, 0};
  f32 s = 1.0f;

  void Apply(const f32 in[3], f32 out[3]) const {
    for (int i = 0; i < 3; ++i) {
      out[i] = (r[i * 3] * in[0] + r[i * 3 + 1] * in[1] + r[i * 3 + 2] * in[2]) * s + t[i];
    }
  }
  void Rotate(const f32 in[3], f32 out[3]) const {
    for (int i = 0; i < 3; ++i) {
      out[i] = r[i * 3] * in[0] + r[i * 3 + 1] * in[1] + r[i * 3 + 2] * in[2];
    }
  }
};

Transform Compose(const Transform& parent, const Transform& local) {
  Transform out;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      out.r[i * 3 + j] = parent.r[i * 3] * local.r[j] + parent.r[i * 3 + 1] * local.r[3 + j] +
                         parent.r[i * 3 + 2] * local.r[6 + j];
    }
  }
  out.s = parent.s * local.s;
  parent.Apply(local.t, out.t);
  return out;
}

// NiAVObject prefix shared by nodes and shapes: name, extra data list,
// controller, flags, transform, collision object. Flag bit 0 is "hidden",
// which collision proxy meshes rely on.
Transform ReadAvObject(Reader& r, bool* hidden) {
  r.Skip(4);  // name string index
  u32 extra_count = r.Read<u32>();
  if (extra_count > 4096) {
    r.ok = false;
    return {};
  }
  r.Skip(4 * extra_count + 4);  // extra refs, controller
  u32 flags = r.Read<u32>();
  if (hidden) *hidden = (flags & 1) != 0;
  Transform local;
  for (f32& v : local.t) v = r.Read<f32>();
  for (f32& v : local.r) v = r.Read<f32>();
  local.s = r.Read<f32>();
  r.Skip(4);  // collision object ref
  return local;
}

struct Node {
  Transform local;
  base::Vector<i32> children;
  bool hidden = false;
};

struct Geometry {
  base::Vector<asset::Vertex> vertices;
  base::Vector<u32> indices;
};

struct Shape {
  Transform local;
  i32 shader = -1;
  i32 alpha = -1;
  i32 data = -1;        // NiTriShape only
  Geometry geometry;    // BSTriShape only
  bool hidden = false;
  bool skipped = false;
};

struct ShaderInfo {
  i32 texture_set = -1;
  u32 flags1 = 0;
  u32 flags2 = 0;
  f32 emissive[3] = {0, 0, 0};
  f32 emissive_multiple = 1;
  f32 glossiness = 80;
  bool effect = false;
  std::string effect_texture;
};

struct AlphaInfo {
  u16 flags = 0;
  u8 threshold = 128;
};

// vertexDesc: nibble 0 is the vertex stride in dwords, nibbles 2..7 are the
// attribute offsets (uv, uv2, normal, tangent, color, skinning) in dwords,
// bits 44+ are presence flags.
struct VertexLayout {
  u32 stride = 0;
  u32 flags = 0;
  u32 uv = 0, normal = 0, tangent = 0, color = 0, skin = 0;
  bool full_precision = false;

  static constexpr u32 kHasVertex = 1 << 0;
  static constexpr u32 kHasUv = 1 << 1;
  static constexpr u32 kHasNormal = 1 << 3;
  static constexpr u32 kHasTangent = 1 << 4;
  static constexpr u32 kHasColor = 1 << 5;
  static constexpr u32 kSkinned = 1 << 6;

  explicit VertexLayout(u64 desc) {
    stride = static_cast<u32>(desc & 0xf) * 4;
    flags = static_cast<u32>(desc >> 44);
    uv = static_cast<u32>(desc >> 8 & 0xf) * 4;
    normal = static_cast<u32>(desc >> 16 & 0xf) * 4;
    tangent = static_cast<u32>(desc >> 20 & 0xf) * 4;
    color = static_cast<u32>(desc >> 24 & 0xf) * 4;
    skin = static_cast<u32>(desc >> 28 & 0xf) * 4;
    u32 position_end = stride;
    for (u32 offset : {uv, normal, tangent, color, skin}) {
      if (offset != 0) position_end = std::min(position_end, offset);
    }
    full_precision = position_end >= 16;
  }
};

bool ReadBsTriShapeGeometry(Reader& r, Geometry* out) {
  u64 desc = r.Read<u64>();
  u32 triangle_count = r.Read<u16>();
  u32 vertex_count = r.Read<u16>();
  u32 data_size = r.Read<u32>();
  if (!r.ok || data_size == 0 || vertex_count == 0) return false;

  VertexLayout layout(desc);
  if (layout.flags & VertexLayout::kSkinned) return false;
  if (!(layout.flags & VertexLayout::kHasVertex) || layout.stride == 0) return false;
  if (data_size != layout.stride * vertex_count + 6 * triangle_count) return false;

  const u8* base = r.Bytes(data_size);
  if (!base) return false;

  out->vertices.resize(vertex_count);
  for (u32 i = 0; i < vertex_count; ++i) {
    const u8* v = base + i * layout.stride;
    asset::Vertex& vertex = out->vertices[i];
    if (layout.full_precision) {
      std::memcpy(vertex.position, v, 12);
    } else {
      u16 h[3];
      std::memcpy(h, v, 6);
      for (int k = 0; k < 3; ++k) vertex.position[k] = HalfToFloat(h[k]);
    }
    if (layout.flags & VertexLayout::kHasUv) {
      u16 h[2];
      std::memcpy(h, v + layout.uv, 4);
      vertex.uv[0] = HalfToFloat(h[0]);
      vertex.uv[1] = HalfToFloat(h[1]);
    }
    if (layout.flags & VertexLayout::kHasNormal) {
      const u8* n = v + layout.normal;
      vertex.normal[0] = ByteToSnorm(n[0]);
      vertex.normal[1] = ByteToSnorm(n[1]);
      vertex.normal[2] = ByteToSnorm(n[2]);
    } else {
      vertex.normal[2] = 1;
    }
    if (layout.flags & VertexLayout::kHasTangent) {
      const u8* t = v + layout.tangent;
      vertex.tangent[0] = ByteToSnorm(t[0]);
      vertex.tangent[1] = ByteToSnorm(t[1]);
      vertex.tangent[2] = ByteToSnorm(t[2]);
      vertex.tangent[3] = 1;
    } else {
      vertex.tangent[0] = 1;
      vertex.tangent[3] = 1;
    }
    if (layout.flags & VertexLayout::kHasColor) {
      std::memcpy(&vertex.color, v + layout.color, 4);
      // Vertex alpha stores wind animation weights on foliage; it must not
      // feed the alpha test.
      vertex.color |= 0xff000000u;
    }
  }

  const u8* tris = base + layout.stride * vertex_count;
  out->indices.resize(triangle_count * 3);
  for (u32 i = 0; i < triangle_count * 3; ++i) {
    u16 index;
    std::memcpy(&index, tris + i * 2, 2);
    if (index >= vertex_count) return false;
    out->indices[i] = index;
  }
  return true;
}

// NiTriShapeData for 20.2.0.7 / BS 100 (the material CRC u32 is SSE only).
bool ReadNiTriShapeData(Reader& r, u32 bs_version, Geometry* out) {
  r.Skip(4);  // group id
  u32 vertex_count = r.Read<u16>();
  r.Skip(2);  // keep/compress flags
  bool has_vertices = r.Read<u8>() != 0;
  if (!r.ok || vertex_count == 0 || !has_vertices) return false;

  out->vertices.resize(vertex_count);
  const u8* positions = r.Bytes(12 * vertex_count);
  if (!positions) return false;
  for (u32 i = 0; i < vertex_count; ++i) {
    std::memcpy(out->vertices[i].position, positions + 12 * i, 12);
  }

  u16 vector_flags = r.Read<u16>();
  if (bs_version == 100) r.Skip(4);  // material CRC
  bool has_normals = r.Read<u8>() != 0;
  if (has_normals) {
    const u8* normals = r.Bytes(12 * vertex_count);
    if (!normals) return false;
    for (u32 i = 0; i < vertex_count; ++i) {
      std::memcpy(out->vertices[i].normal, normals + 12 * i, 12);
    }
    if (vector_flags & 0x1000) {
      const u8* tangents = r.Bytes(12 * vertex_count);
      r.Skip(12 * vertex_count);  // bitangents
      if (!tangents) return false;
      for (u32 i = 0; i < vertex_count; ++i) {
        std::memcpy(out->vertices[i].tangent, tangents + 12 * i, 12);
        out->vertices[i].tangent[3] = 1;
      }
    }
  }
  r.Skip(16);  // center + radius
  bool has_colors = r.Read<u8>() != 0;
  if (has_colors) {
    const u8* colors = r.Bytes(16 * vertex_count);
    if (!colors) return false;
    for (u32 i = 0; i < vertex_count; ++i) {
      f32 c[4];
      std::memcpy(c, colors + 16 * i, 16);
      auto pack = [](f32 v) { return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
      // Alpha dropped, see the BSTriShape path.
      out->vertices[i].color = pack(c[0]) | pack(c[1]) << 8 | pack(c[2]) << 16 | 0xffu << 24;
    }
  }
  u32 uv_sets = vector_flags & 0x3f;
  if (uv_sets > 0) {
    const u8* uvs = r.Bytes(8 * vertex_count);  // first set only
    if (!uvs) return false;
    for (u32 i = 0; i < vertex_count; ++i) {
      std::memcpy(out->vertices[i].uv, uvs + 8 * i, 8);
    }
    if (uv_sets > 1) r.Skip(8 * vertex_count * (uv_sets - 1));
  }
  r.Skip(2 + 4);  // consistency flags, additional data ref
  u32 triangle_count = r.Read<u16>();
  r.Skip(4);  // num triangle points
  bool has_triangles = r.Read<u8>() != 0;
  if (!r.ok || !has_triangles) return false;
  const u8* tris = r.Bytes(6 * triangle_count);
  if (!tris) return false;
  out->indices.resize(triangle_count * 3);
  for (u32 i = 0; i < triangle_count * 3; ++i) {
    u16 index;
    std::memcpy(&index, tris + i * 2, 2);
    if (index >= vertex_count) return false;
    out->indices[i] = index;
  }
  return true;
}

std::string ReadSizedString(Reader& r) {
  u32 length = r.Read<u32>();
  if (length > 4096) {
    r.ok = false;
    return {};
  }
  const u8* bytes = r.Bytes(length);
  if (!bytes) return {};
  return std::string(reinterpret_cast<const char*>(bytes), length);
}

std::string NormalizeTexturePath(std::string_view raw) {
  if (raw.empty()) return {};
  std::string path = asset::NormalizePath(raw);
  // Source art paths leak build prefixes like "skyrimhd/build/pc/data/
  // textures/..."; the vfs root is the last "textures/" segment.
  size_t anchor = path.rfind("textures/");
  if (anchor != std::string::npos) return path.substr(anchor);
  return "textures/" + path;
}

}  // namespace

std::optional<NifHeader> ParseNifHeader(ByteSpan data) {
  std::string_view text(reinterpret_cast<const char*>(data.data()),
                        std::min<size_t>(data.size(), 64));
  if (!text.starts_with(kMagic)) return std::nullopt;
  size_t newline = text.find('\n');
  if (newline == std::string_view::npos) return std::nullopt;

  Reader r{data, newline + 1};
  NifHeader header;
  header.version = r.Read<u32>();
  if (header.version != kVersion20_2_0_7) return std::nullopt;
  if (r.Read<u8>() != 1) return std::nullopt;  // little endian only
  header.user_version = r.Read<u32>();
  u32 block_count = r.Read<u32>();
  if (header.user_version >= 12) {
    header.bs_version = r.Read<u32>();
    int export_strings = header.bs_version >= 130 ? 4 : 3;
    for (int i = 0; i < export_strings; ++i) r.Skip(r.Read<u8>());
  }
  u16 type_count = r.Read<u16>();
  if (!r.ok || block_count > 200000 || type_count > 4096) return std::nullopt;
  header.block_types.reserve(type_count);
  for (u16 i = 0; i < type_count; ++i) header.block_types.push_back(ReadSizedString(r));
  header.block_type_index.resize(block_count);
  for (u32 i = 0; i < block_count; ++i) header.block_type_index[i] = r.Read<u16>() & 0x7fff;
  header.block_sizes.resize(block_count);
  for (u32 i = 0; i < block_count; ++i) header.block_sizes[i] = r.Read<u32>();
  u32 string_count = r.Read<u32>();
  r.Skip(4);  // max string length
  if (!r.ok || string_count > 200000) return std::nullopt;
  for (u32 i = 0; i < string_count; ++i) ReadSizedString(r);
  u32 group_count = r.Read<u32>();
  r.Skip(4 * static_cast<size_t>(group_count));
  if (!r.ok) return std::nullopt;

  header.block_offsets.resize(block_count);
  size_t pos = r.pos;
  for (u32 i = 0; i < block_count; ++i) {
    header.block_offsets[i] = static_cast<u32>(pos);
    pos += header.block_sizes[i];
    if (pos > data.size()) return std::nullopt;
  }
  return header;
}

NifConversion ConvertNifScene(ByteSpan data, asset::AssetId id, std::string_view source_path) {
  NifConversion result;
  auto header = ParseNifHeader(data);
  if (!header) {
    REC_WARN("unsupported nif: {}", source_path);
    return result;
  }

  u32 block_count = static_cast<u32>(header->block_sizes.size());
  base::UnorderedMap<u32, Node> nodes;
  base::UnorderedMap<u32, Shape> shapes;
  base::UnorderedMap<u32, Geometry> geometry_blocks;
  base::UnorderedMap<u32, ShaderInfo> shaders;
  base::UnorderedMap<u32, AlphaInfo> alphas;
  base::UnorderedMap<u32, base::Vector<std::string>> texture_sets;

  for (u32 i = 0; i < block_count; ++i) {
    const std::string& type = header->block_types[header->block_type_index[i]];
    Reader r{data.subspan(header->block_offsets[i], header->block_sizes[i])};

    if (type.ends_with("Node")) {
      Node node;
      node.local = ReadAvObject(r, &node.hidden);
      u32 child_count = r.Read<u32>();
      if (!r.ok || child_count > 65536) continue;
      node.children.reserve(child_count);
      for (u32 c = 0; c < child_count; ++c) node.children.push_back(r.Read<i32>());
      if (r.ok) nodes.emplace(i, std::move(node));
    } else if (type == "BSTriShape" || type == "BSMeshLODTriShape") {
      Shape shape;
      shape.local = ReadAvObject(r, &shape.hidden);
      r.Skip(16);  // bounding sphere
      r.Skip(4);   // skin ref
      shape.shader = r.Read<i32>();
      shape.alpha = r.Read<i32>();
      if (!r.ok) continue;
      if (!ReadBsTriShapeGeometry(r, &shape.geometry)) shape.skipped = true;
      shapes.emplace(i, std::move(shape));
    } else if (type == "NiTriShape") {
      Shape shape;
      shape.local = ReadAvObject(r, &shape.hidden);
      shape.data = r.Read<i32>();
      i32 skin = r.Read<i32>();
      u32 material_count = r.Read<u32>();
      if (!r.ok || material_count > 4096) continue;
      r.Skip(8 * material_count + 4 + 1);  // names+extra, active material, needs update
      shape.shader = r.Read<i32>();
      shape.alpha = r.Read<i32>();
      if (!r.ok) continue;
      if (skin >= 0) shape.skipped = true;
      shapes.emplace(i, std::move(shape));
    } else if (type == "NiTriShapeData") {
      Geometry geometry;
      if (ReadNiTriShapeData(r, header->bs_version, &geometry)) {
        geometry_blocks.emplace(i, std::move(geometry));
      }
    } else if (type == "NiTriStrips" || type == "BSDynamicTriShape") {
      ++result.skipped_shapes;
    } else if (type == "BSLightingShaderProperty") {
      ShaderInfo info;
      r.Skip(4);  // shader type
      r.Skip(4);  // name
      u32 extra = r.Read<u32>();
      if (!r.ok || extra > 4096) continue;
      r.Skip(4 * extra + 4);  // extra refs, controller
      info.flags1 = r.Read<u32>();
      info.flags2 = r.Read<u32>();
      r.Skip(16);  // uv offset + scale
      info.texture_set = r.Read<i32>();
      for (f32& v : info.emissive) v = r.Read<f32>();
      info.emissive_multiple = r.Read<f32>();
      r.Skip(4 + 4 + 4);  // clamp mode, alpha, refraction strength
      info.glossiness = r.Read<f32>();
      if (r.ok) shaders.emplace(i, info);
    } else if (type == "BSEffectShaderProperty") {
      ShaderInfo info;
      info.effect = true;
      r.Skip(4);  // name
      u32 extra = r.Read<u32>();
      if (!r.ok || extra > 4096) continue;
      r.Skip(4 * extra + 4);  // extra refs, controller
      info.flags1 = r.Read<u32>();
      info.flags2 = r.Read<u32>();
      r.Skip(16);  // uv offset + scale
      info.effect_texture = ReadSizedString(r);
      if (r.ok) shaders.emplace(i, std::move(info));
    } else if (type == "BSShaderTextureSet") {
      u32 count = r.Read<u32>();
      if (!r.ok || count > 32) continue;
      base::Vector<std::string> textures;
      textures.reserve(count);
      for (u32 t = 0; t < count; ++t) textures.push_back(ReadSizedString(r));
      if (r.ok) texture_sets.emplace(i, std::move(textures));
    } else if (type == "NiAlphaProperty") {
      AlphaInfo info;
      r.Skip(4);  // name
      u32 extra = r.Read<u32>();
      if (!r.ok || extra > 4096) continue;
      r.Skip(4 * extra + 4);  // extra refs, controller
      info.flags = r.Read<u16>();
      info.threshold = r.Read<u8>();
      if (r.ok) alphas.emplace(i, info);
    }
  }

  // Footer: root refs follow the last block.
  base::Vector<u32> roots;
  {
    size_t footer = header->block_offsets.empty()
                        ? 0
                        : header->block_offsets[block_count - 1] + header->block_sizes[block_count - 1];
    Reader r{data, footer};
    u32 root_count = r.Read<u32>();
    if (r.ok && root_count < 256) {
      for (u32 i = 0; i < root_count; ++i) {
        i32 root = r.Read<i32>();
        if (r.ok && root >= 0) roots.push_back(static_cast<u32>(root));
      }
    }
    if (roots.empty()) roots.push_back(0);
  }

  auto mesh = base::MakeUnique<asset::Mesh>();
  mesh->id = id;
  mesh->lods.emplace_back();
  asset::MeshLod& lod = mesh->lods[0];

  // Material per distinct (shader, alpha) pair.
  base::UnorderedMap<u64, asset::AssetId> material_ids;
  auto material_for = [&](i32 shader_block, i32 alpha_block) -> asset::AssetId {
    u64 key = static_cast<u64>(static_cast<u32>(shader_block)) << 32 | static_cast<u32>(alpha_block);
    if (asset::AssetId* known = material_ids.find(key)) return *known;

    asset::Material material;
    std::string name = std::string(source_path) + "#m" +
                       std::to_string(result.materials.size());
    material.id = asset::MakeAssetId(name);

    const ShaderInfo* shader = shaders.find(static_cast<u32>(shader_block));
    if (shader) {
      std::string diffuse, normal;
      if (shader->effect) {
        diffuse = NormalizeTexturePath(shader->effect_texture);
        material.alpha_mode = asset::AlphaMode::kBlend;
      } else if (const auto* set = texture_sets.find(static_cast<u32>(shader->texture_set))) {
        if (set->size() > 0) diffuse = NormalizeTexturePath((*set)[0]);
        if (set->size() > 1) normal = NormalizeTexturePath((*set)[1]);
      }
      if (!diffuse.empty()) {
        material.base_color = asset::MakeAssetId(diffuse);
        if (std::ranges::find(result.texture_paths, diffuse) == result.texture_paths.end()) {
          result.texture_paths.push_back(std::move(diffuse));
        }
      }
      if (!normal.empty()) {
        material.normal = asset::MakeAssetId(normal);
        if (std::ranges::find(result.texture_paths, normal) == result.texture_paths.end()) {
          result.texture_paths.push_back(std::move(normal));
        }
      }
      material.two_sided = (shader->flags2 & 0x10) != 0;
      material.metallic_factor = 0;
      // Specular power to perceptual roughness, Karis' approximation.
      f32 gloss = std::clamp(shader->glossiness, 1.0f, 1000.0f);
      material.roughness_factor = std::clamp(std::pow(2.0f / (gloss + 2.0f), 0.25f), 0.2f, 1.0f);
      constexpr u32 kShaderFlag1OwnEmit = 1u << 22;
      if (!shader->effect && (shader->flags1 & kShaderFlag1OwnEmit)) {
        for (int k = 0; k < 3; ++k) {
          material.emissive_factor[k] = shader->emissive[k] * shader->emissive_multiple;
        }
      }
    }
    if (const AlphaInfo* alpha = alphas.find(static_cast<u32>(alpha_block))) {
      if (alpha->flags & 0x0001) {
        material.alpha_mode = asset::AlphaMode::kBlend;
      } else if (alpha->flags & 0x0200) {
        material.alpha_mode = asset::AlphaMode::kMask;
        material.alpha_cutoff = static_cast<f32>(alpha->threshold) / 255.0f;
      }
    }

    result.materials.push_back(material);
    material_ids.emplace(key, material.id);
    return material.id;
  };

  // Flatten: depth first from the roots, baking world transforms.
  struct StackEntry {
    u32 block;
    Transform world;
  };
  base::Vector<StackEntry> stack;
  for (u32 root : roots) stack.push_back({root, Transform{}});
  base::Vector<u8> visited(block_count);

  f32 bounds_min[3] = {1e30f, 1e30f, 1e30f};
  f32 bounds_max[3] = {-1e30f, -1e30f, -1e30f};

  while (!stack.empty()) {
    StackEntry entry = stack.back();
    stack.pop_back();
    if (entry.block >= block_count || visited[entry.block]) continue;
    visited[entry.block] = true;

    if (const Node* node = nodes.find(entry.block)) {
      if (node->hidden) continue;
      Transform world = Compose(entry.world, node->local);
      for (i32 child : node->children) {
        if (child >= 0) stack.push_back({static_cast<u32>(child), world});
      }
      continue;
    }

    Shape* shape = shapes.find(entry.block);
    if (!shape || shape->hidden) continue;
    // Effect shader geometry (smoke wisps, god rays, glow planes) needs
    // blending the mesh path does not do yet; opaque it is worse than absent.
    if (const ShaderInfo* shader = shaders.find(static_cast<u32>(shape->shader));
        shader && shader->effect) {
      ++result.skipped_shapes;
      continue;
    }
    const Geometry* geometry = &shape->geometry;
    if (shape->data >= 0) {
      geometry = geometry_blocks.find(static_cast<u32>(shape->data));
      if (!geometry) shape->skipped = true;
    }
    if (shape->skipped || !geometry || geometry->vertices.empty() || geometry->indices.empty()) {
      if (shape->skipped) ++result.skipped_shapes;
      continue;
    }

    Transform world = Compose(entry.world, shape->local);
    u32 vertex_base = static_cast<u32>(lod.vertices.size());
    u32 index_offset = static_cast<u32>(lod.indices.size());
    for (const asset::Vertex& src : geometry->vertices) {
      asset::Vertex v = src;
      world.Apply(src.position, v.position);
      world.Rotate(src.normal, v.normal);
      world.Rotate(src.tangent, v.tangent);
      for (int k = 0; k < 3; ++k) {
        bounds_min[k] = std::min(bounds_min[k], v.position[k]);
        bounds_max[k] = std::max(bounds_max[k], v.position[k]);
      }
      lod.vertices.push_back(v);
    }
    for (u32 index : geometry->indices) lod.indices.push_back(vertex_base + index);

    asset::Submesh submesh;
    submesh.index_offset = index_offset;
    submesh.index_count = static_cast<u32>(geometry->indices.size());
    submesh.material = material_for(shape->shader, shape->alpha);
    lod.submeshes.push_back(submesh);
  }

  if (lod.vertices.empty()) {
    result.mesh = nullptr;
    return result;
  }

  for (int k = 0; k < 3; ++k) mesh->bounds_center[k] = (bounds_min[k] + bounds_max[k]) * 0.5f;
  f32 radius_sq = 0;
  for (int k = 0; k < 3; ++k) {
    f32 d = bounds_max[k] - mesh->bounds_center[k];
    radius_sq += d * d;
  }
  mesh->bounds_radius = std::sqrt(radius_sq);
  result.mesh = std::move(mesh);
  return result;
}

}  // namespace rec::bethesda
