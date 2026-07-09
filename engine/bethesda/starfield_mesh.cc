#include "bethesda/starfield_mesh.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>

#include <base/containers/unordered_map.h>

#include "bethesda/nif.h"
#include "core/log.h"

namespace rx::bethesda {
namespace {

// Starfield ".mesh" positions are in metres (a small rock is ~0.6 units wide),
// where Skyrim/Fallout meshes are in Bethesda game units (~70 per metre) and the
// shared cell streamer multiplies object-space positions by kUnitsToMeters
// (0.01428 == 1/70) to place them. Scaling Starfield positions up by the
// reciprocal lands them on the same game-unit convention the rest of the
// pipeline expects.
constexpr f32 kMetresToGameUnits = 70.0f;

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
    bits = sign << 31;  // subnormals flush to zero, irrelevant at uv scale
  } else if (exponent == 31) {
    return 0;  // inf/nan in source data, neutralize
  } else {
    bits = sign << 31 | (exponent + 112) << 23 | mantissa << 13;
  }
  f32 out;
  std::memcpy(&out, &bits, 4);
  return out;
}

// p' = rotation * p * scale + translation, rotation rows in file order. Mirrors
// the NIF node transform convention in nif.cc.
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

// NiAVObject prefix shared by NiNode and BSGeometry: name, extra data list,
// controller, flags, transform, collision object. Flag bit 0 is "hidden".
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

// "<dir>\<file>" (20 hex + '\' + 20 hex) -> "geometries/<dir>/<file>.mesh".
std::string MeshPathFromHash(std::string_view raw) {
  std::string path = "geometries/";
  path.reserve(raw.size() + 17);
  for (char c : raw) path.push_back(c == '\\' ? '/' : static_cast<char>(std::tolower(c)));
  path += ".mesh";
  return path;
}

// True for a 40-char "<20 hex>\<20 hex>" geometry hash reference.
bool IsHashPath(std::string_view s) {
  if (s.size() != 41 || s[20] != '\\') return false;
  for (size_t i = 0; i < s.size(); ++i) {
    if (i == 20) continue;
    char c = s[i];
    bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!hex) return false;
  }
  return true;
}

// Reads a BSGeometry block: the NiAVObject prefix (gives the node transform),
// bounds, then the LOD table referencing external ".mesh" files. Returns the
// highest detail (first) mesh path, or empty if none. The LOD table format
// varies for skinned geometry, so the structured parse falls back to scanning
// the block for the hash-path pattern, which is unambiguous.
std::string ReadBsGeometry(Reader& r, Transform* local) {
  *local = ReadAvObject(r, nullptr);
  r.Skip(10 * 4);  // bounding sphere (center + radius) and box (center + half extents)
  r.Read<u32>();   // 0xffffffff sentinel
  u32 lod_count = r.Read<u32>();
  r.Read<u32>();   // material slot count
  if (r.ok && lod_count <= 64) {
    for (u32 i = 0; i < lod_count; ++i) {
      u8 present = r.Read<u8>();
      if (present == 0) continue;
      r.Skip(12);  // per-lod meshlet/vertex counts and a constant
      u32 path_len = r.Read<u32>();
      if (!r.ok || path_len > 256) break;
      const u8* bytes = r.Bytes(path_len);
      if (!bytes) break;
      std::string_view path(reinterpret_cast<const char*>(bytes), path_len);
      if (IsHashPath(path)) return MeshPathFromHash(path);
    }
  }

  // Structured parse came up empty (e.g. a skinned geometry header variant):
  // scan the whole block for the first hash-path reference.
  std::string_view block(reinterpret_cast<const char*>(r.data.data()), r.data.size());
  for (size_t i = 0; i + 41 <= block.size(); ++i) {
    std::string_view candidate = block.substr(i, 41);
    if (IsHashPath(candidate)) return MeshPathFromHash(candidate);
  }
  return {};
}

struct Node {
  Transform local;
  base::Vector<i32> children;
  bool hidden = false;
};

struct GeometryRef {
  Transform local;
  std::string mesh_path;
  bool hidden = false;
};

// Recomputes area-weighted smooth vertex normals from the geometry. The packed
// ".mesh" normal stream uses an undecoded format, so until it is reversed the
// geometric normals give correct lighting (sharp edges soften, acceptable for a
// first cut).
void ComputeNormals(StarfieldMeshData* mesh) {
  for (asset::Vertex& v : mesh->vertices) {
    v.normal[0] = v.normal[1] = v.normal[2] = 0;
  }
  for (size_t i = 0; i + 2 < mesh->indices.size(); i += 3) {
    u32 a = mesh->indices[i], b = mesh->indices[i + 1], c = mesh->indices[i + 2];
    if (a >= mesh->vertices.size() || b >= mesh->vertices.size() ||
        c >= mesh->vertices.size()) {
      continue;
    }
    const f32* pa = mesh->vertices[a].position;
    const f32* pb = mesh->vertices[b].position;
    const f32* pc = mesh->vertices[c].position;
    f32 e0[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
    f32 e1[3] = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
    f32 n[3] = {e0[1] * e1[2] - e0[2] * e1[1], e0[2] * e1[0] - e0[0] * e1[2],
                e0[0] * e1[1] - e0[1] * e1[0]};
    for (u32 idx : {a, b, c}) {
      for (int k = 0; k < 3; ++k) mesh->vertices[idx].normal[k] += n[k];
    }
  }
  for (asset::Vertex& v : mesh->vertices) {
    f32 len = std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] +
                        v.normal[2] * v.normal[2]);
    if (len > 1e-8f) {
      for (int k = 0; k < 3; ++k) v.normal[k] /= len;
    } else {
      v.normal[0] = v.normal[1] = 0;
      v.normal[2] = 1;
    }
  }
}

}  // namespace

bool ParseStarfieldMesh(ByteSpan data, StarfieldMeshData* out) {
  *out = StarfieldMeshData{};
  Reader r{data};
  u32 version = r.Read<u32>();
  if (!r.ok || (version != 1 && version != 2)) return false;

  u32 index_count = r.Read<u32>();
  if (!r.ok || index_count == 0 || index_count % 3 != 0 || index_count > 64'000'000) {
    return false;
  }
  const u8* index_bytes = r.Bytes(static_cast<size_t>(index_count) * 2);
  if (!index_bytes) return false;

  f32 scale = r.Read<f32>();
  r.Read<u32>();  // weights per vertex, geometry is baked rigid here
  u32 vertex_count = r.Read<u32>();
  if (!r.ok || vertex_count == 0 || vertex_count > 16'000'000) return false;
  const u8* position_bytes = r.Bytes(static_cast<size_t>(vertex_count) * 6);
  if (!position_bytes) return false;

  out->indices.resize(index_count);
  for (u32 i = 0; i < index_count; ++i) {
    u16 index;
    std::memcpy(&index, index_bytes + i * 2, 2);
    if (index >= vertex_count) return false;
    out->indices[i] = index;
  }

  out->vertices.resize(vertex_count);
  // Positions are i16 quantized over [-scale, scale], in metres; lift them to
  // the Bethesda game-unit object space the streamer scales back down.
  constexpr f32 kQuant = 1.0f / 32767.0f * kMetresToGameUnits;
  for (u32 i = 0; i < vertex_count; ++i) {
    i16 p[3];
    std::memcpy(p, position_bytes + i * 6, 6);
    asset::Vertex& v = out->vertices[i];
    for (int k = 0; k < 3; ++k) v.position[k] = static_cast<f32>(p[k]) * kQuant * scale;
    v.tangent[0] = 1;
    v.tangent[3] = 1;
  }

  // Optional per-vertex streams in a fixed order: each is a u32 count followed
  // by count * 4 bytes. count == 0 means the stream is absent. Only the first uv
  // and the vertex color feed the engine vertex; normals/tangents are recomputed
  // from geometry because the packed format is not yet decoded.
  auto read_stream = [&](u32 stride, const std::function<void(u32, const u8*)>& consume) {
    u32 count = r.Read<u32>();
    if (!r.ok) return;
    if (count == 0) return;
    const u8* bytes = r.Bytes(static_cast<size_t>(count) * stride);
    if (!bytes) return;
    if (count == vertex_count && consume) consume(count, bytes);
  };

  read_stream(4, [&](u32 count, const u8* bytes) {  // uv1, two float16
    for (u32 i = 0; i < count; ++i) {
      u16 h[2];
      std::memcpy(h, bytes + i * 4, 4);
      out->vertices[i].uv[0] = HalfToFloat(h[0]);
      out->vertices[i].uv[1] = HalfToFloat(h[1]);
    }
  });
  read_stream(4, nullptr);                          // uv2, unused
  read_stream(4, [&](u32 count, const u8* bytes) {  // vertex color, RGBA8
    for (u32 i = 0; i < count; ++i) {
      std::memcpy(&out->vertices[i].color, bytes + i * 4, 4);
      out->vertices[i].color |= 0xff000000u;  // drop wind-weight alpha
    }
  });
  read_stream(4, nullptr);  // packed normals, format undecoded
  read_stream(4, nullptr);  // packed tangents, format undecoded
  if (!r.ok) return false;

  ComputeNormals(out);
  return true;
}

namespace {

// Recomputes smooth normals on a skinned buffer, sharing the rigid path's
// algorithm by reading the same Vertex/index fields through a temporary view.
void ComputeNormalsSkinned(StarfieldSkinnedMeshData* mesh) {
  StarfieldMeshData view;
  view.vertices = std::move(mesh->vertices);
  view.indices = mesh->indices;
  ComputeNormals(&view);
  mesh->vertices = std::move(view.vertices);
}

}  // namespace

bool ParseStarfieldSkinnedMesh(ByteSpan data, StarfieldSkinnedMeshData* out) {
  *out = StarfieldSkinnedMeshData{};
  Reader r{data};
  u32 version = r.Read<u32>();
  if (!r.ok || (version != 1 && version != 2)) return false;

  u32 index_count = r.Read<u32>();
  if (!r.ok || index_count == 0 || index_count % 3 != 0 || index_count > 64'000'000) {
    return false;
  }
  const u8* index_bytes = r.Bytes(static_cast<size_t>(index_count) * 2);
  if (!index_bytes) return false;

  f32 scale = r.Read<f32>();
  u32 weights_per_vertex = r.Read<u32>();
  u32 vertex_count = r.Read<u32>();
  if (!r.ok || vertex_count == 0 || vertex_count > 16'000'000) return false;
  if (weights_per_vertex == 0 || weights_per_vertex > 8) return false;  // rigid mesh
  const u8* position_bytes = r.Bytes(static_cast<size_t>(vertex_count) * 6);
  if (!position_bytes) return false;

  out->indices.resize(index_count);
  for (u32 i = 0; i < index_count; ++i) {
    u16 index;
    std::memcpy(&index, index_bytes + i * 2, 2);
    if (index >= vertex_count) return false;
    out->indices[i] = index;
  }

  out->vertices.resize(vertex_count);
  // Positions stay in Starfield-native metres (no game-unit lift): the skinned
  // body is posed by a metres-space skeleton and metres-space bind transforms,
  // so keeping the vertices in metres keeps the whole skin chain consistent.
  const f32 quant = 1.0f / 32767.0f * scale;
  for (u32 i = 0; i < vertex_count; ++i) {
    i16 p[3];
    std::memcpy(p, position_bytes + i * 6, 6);
    asset::Vertex& v = out->vertices[i];
    for (int k = 0; k < 3; ++k) v.position[k] = static_cast<f32>(p[k]) * quant;
    v.tangent[0] = 1;
    v.tangent[3] = 1;
  }

  // The five optional vertex streams, same fixed order as the rigid path. The
  // weight stream follows the last of them, so they must all be consumed even
  // when unused, to leave the reader at the weight stream.
  auto read_stream = [&](u32 stride, const std::function<void(u32, const u8*)>& consume) {
    u32 count = r.Read<u32>();
    if (!r.ok) return;
    if (count == 0) return;
    const u8* bytes = r.Bytes(static_cast<size_t>(count) * stride);
    if (!bytes) return;
    if (count == vertex_count && consume) consume(count, bytes);
  };
  read_stream(4, [&](u32 count, const u8* bytes) {  // uv1, two float16
    for (u32 i = 0; i < count; ++i) {
      u16 h[2];
      std::memcpy(h, bytes + i * 4, 4);
      out->vertices[i].uv[0] = HalfToFloat(h[0]);
      out->vertices[i].uv[1] = HalfToFloat(h[1]);
    }
  });
  read_stream(4, nullptr);                          // uv2, unused
  read_stream(4, [&](u32 count, const u8* bytes) {  // vertex color, RGBA8
    for (u32 i = 0; i < count; ++i) {
      std::memcpy(&out->vertices[i].color, bytes + i * 4, 4);
      out->vertices[i].color |= 0xff000000u;
    }
  });
  read_stream(4, nullptr);  // packed normals, format undecoded
  read_stream(4, nullptr);  // packed tangents, format undecoded
  if (!r.ok) return false;

  // Weight stream: u32 influenceCount == vertexCount * weightsPerVertex, then
  // that many (u16 boneIndex, u16 weight) entries grouped per vertex. Reduce
  // each vertex's influences to its four largest, renormalized to u8 weights
  // summing to 255 (the engine SkinnedVertexExtra holds four influences).
  u32 influence_count = r.Read<u32>();
  if (!r.ok || influence_count != vertex_count * weights_per_vertex) return false;
  const u8* influence_bytes = r.Bytes(static_cast<size_t>(influence_count) * 4);
  if (!influence_bytes) return false;

  out->skinning.resize(vertex_count);
  for (u32 v = 0; v < vertex_count; ++v) {
    // Pick the four highest-weight influences of this vertex by a small partial
    // selection (weightsPerVertex is tiny, so a linear pass per slot is fine).
    u16 picked_bone[4] = {0, 0, 0, 0};
    u32 picked_weight[4] = {0, 0, 0, 0};
    bool used[8] = {};
    const u8* group = influence_bytes + static_cast<size_t>(v) * weights_per_vertex * 4;
    for (int slot = 0; slot < 4 && slot < static_cast<int>(weights_per_vertex); ++slot) {
      int best = -1;
      u16 best_weight = 0;
      for (u32 j = 0; j < weights_per_vertex; ++j) {
        if (used[j]) continue;
        u16 weight;
        std::memcpy(&weight, group + j * 4 + 2, 2);
        if (best < 0 || weight > best_weight) {
          best = static_cast<int>(j);
          best_weight = weight;
        }
      }
      if (best < 0) break;
      used[best] = true;
      std::memcpy(&picked_bone[slot], group + best * 4, 2);
      picked_weight[slot] = best_weight;
    }

    u32 total = picked_weight[0] + picked_weight[1] + picked_weight[2] + picked_weight[3];
    asset::SkinnedVertexExtra& extra = out->skinning[v];
    if (total == 0) {
      extra.bone_indices[0] = static_cast<u8>(picked_bone[0]);
      extra.bone_weights[0] = 255;
      continue;
    }
    // Renormalize to u8 summing to 255, putting the rounding remainder on the
    // largest (slot 0) so the dominant bone absorbs it.
    u32 assigned = 0;
    for (int slot = 0; slot < 4; ++slot) {
      extra.bone_indices[slot] = static_cast<u8>(picked_bone[slot]);
      u8 w = static_cast<u8>((static_cast<u64>(picked_weight[slot]) * 255 + total / 2) / total);
      extra.bone_weights[slot] = w;
      assigned += w;
    }
    if (assigned != 255) {
      int delta = 255 - static_cast<int>(assigned);
      extra.bone_weights[0] = static_cast<u8>(std::clamp(extra.bone_weights[0] + delta, 0, 255));
    }
  }

  ComputeNormalsSkinned(out);
  return true;
}

bool ParseStarfieldNif(ByteSpan data, base::Vector<StarfieldGeometryRef>* out) {
  auto header = ParseNifHeader(data);
  if (!header) return false;
  u32 block_count = static_cast<u32>(header->block_sizes.size());

  base::UnorderedMap<u32, Node> nodes;
  base::UnorderedMap<u32, GeometryRef> geometries;
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
    } else if (type == "BSGeometry") {
      GeometryRef geo;
      geo.mesh_path = ReadBsGeometry(r, &geo.local);
      geo.hidden = false;
      if (!geo.mesh_path.empty()) geometries.emplace(i, std::move(geo));
    }
  }
  if (geometries.empty()) return false;

  base::Vector<u32> roots;
  {
    size_t footer = header->block_offsets.empty()
                        ? 0
                        : header->block_offsets[block_count - 1] +
                              header->block_sizes[block_count - 1];
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

  // Depth first from the roots, baking the node-chain transform down to each
  // BSGeometry, exactly as the BSTriShape path bakes node transforms.
  struct StackEntry {
    u32 block;
    Transform world;
  };
  base::Vector<StackEntry> stack;
  for (u32 root : roots) stack.push_back({root, Transform{}});
  base::Vector<u8> visited(block_count);
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
    const GeometryRef* geo = geometries.find(entry.block);
    if (!geo || geo->hidden) continue;
    Transform world = Compose(entry.world, geo->local);
    StarfieldGeometryRef ref;
    std::memcpy(ref.rotation, world.r, sizeof(ref.rotation));
    std::memcpy(ref.translation, world.t, sizeof(ref.translation));
    ref.scale = world.s;
    ref.mesh_path = geo->mesh_path;
    out->push_back(std::move(ref));
  }
  return !out->empty();
}

}  // namespace rx::bethesda
