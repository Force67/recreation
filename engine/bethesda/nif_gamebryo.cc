// Classic Gamebryo NIF reader (Oblivion: 20.0.0.4/20.0.0.5, plus the 10.1/10.2
// stragglers the game ships). These files predate the Bethesda stream header
// and, crucially, the per-block size table (added in 20.2.0.5), so the file
// can only be walked by parsing every block type it contains in sequence.
// Layouts follow niftools nif.xml and were byte-validated against the real
// Oblivion BSAs (exact-consumption over ~3000 world meshes).
//
// Geometry lives in NiTriShape/NiTriStrips with inline NiTexturingProperty /
// NiMaterialProperty / NiAlphaProperty instead of the later shader property
// blocks. Havok collision (bhk*), animation controllers and extra data are
// parsed structurally so the walk can pass over them; particle systems and
// skinning are not supported yet and fail the file (the converter skips it).

#include "bethesda/nif.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <base/containers/unordered_map.h>

#include "asset/asset_id.h"
#include "core/log.h"

namespace rx::bethesda {
namespace {

constexpr u32 kV4_0_0_2 = 0x04000002;  // Morrowind (NetImmerse)
constexpr u32 kV10_0_1_2 = 0x0A000102;
constexpr u32 kV10_1_0_0 = 0x0A010000;
constexpr u32 kV10_1_0_103 = 0x0A010067;
constexpr u32 kV10_1_0_106 = 0x0A01006A;
constexpr u32 kV10_1_0_114 = 0x0A010072;
constexpr u32 kV10_2_0_0 = 0x0A020000;
constexpr u32 kV20_0_0_3 = 0x14000003;
constexpr u32 kV20_0_0_4 = 0x14000004;

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

// Booleans are 4 bytes up to 4.0.0.2, 1 byte from 4.1 on.
bool ReadBool(Reader& r, u32 version) {
  return version <= kV4_0_0_2 ? r.Read<u32>() != 0 : r.Read<u8>() != 0;
}

// Same convention as the 20.2.0.7 reader: rotation rows in file order.
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

std::string NormalizeTexturePath(std::string_view raw) {
  if (raw.empty()) return {};
  std::string path = asset::NormalizePath(raw);
  size_t anchor = path.rfind("textures/");
  if (anchor != std::string::npos) return path.substr(anchor);
  return "textures/" + path;
}

struct GbNode {
  Transform local;
  base::Vector<i32> children;
  base::Vector<i32> properties;
  bool hidden = false;
};

struct GbShape {
  Transform local;
  std::string name;
  base::Vector<i32> extra;
  base::Vector<i32> properties;
  i32 data = -1;
  i32 skin = -1;
  bool hidden = false;
};

struct GbGeometry {
  base::Vector<asset::Vertex> vertices;
  base::Vector<u32> indices;
};

struct GbTexturing {
  i32 base_source = -1;
  i32 glow_source = -1;
};

struct GbMaterial {
  f32 emissive[3] = {0, 0, 0};
  f32 glossiness = 10;
  f32 alpha = 1;
};

struct GbAlpha {
  u16 flags = 0;
  u8 threshold = 128;
};

// Tangent space stored as NiBinaryExtraData off the shape: numVertices Vector3
// tangents followed by numVertices Vector3 bitangents.
struct GbTangents {
  const u8* data = nullptr;
  size_t size = 0;
};

struct GbScene {
  u32 version = 0;
  base::Vector<std::string> block_types;   // resolved per block
  base::Vector<i32> roots;
  base::UnorderedMap<u32, GbNode> nodes;
  base::UnorderedMap<u32, GbShape> shapes;
  base::UnorderedMap<u32, GbGeometry> geometry;
  base::UnorderedMap<u32, GbTexturing> texturing;
  base::UnorderedMap<u32, std::string> textures;  // NiSourceTexture file names
  base::UnorderedMap<u32, GbMaterial> materials;
  base::UnorderedMap<u32, GbAlpha> alphas;
  base::UnorderedMap<u32, u32> stencil_draw_mode;
  base::UnorderedMap<u32, GbTangents> tangents;  // NiBinaryExtraData blocks
};

// ---- shared block prefixes ----

struct AvObjectPrefix {
  Transform local;
  std::string name;
  base::Vector<i32> extra;
  base::Vector<i32> properties;
  bool hidden = false;
};

void ReadObjectNet(Reader& r, u32 version, std::string* name, base::Vector<i32>* extra) {
  std::string n = ReadSizedString(r);
  if (name) *name = std::move(n);
  if (version <= kV4_0_0_2) {
    // Pre-10.x extra data is a single chained ref, not a list.
    i32 ref = r.Read<i32>();
    if (extra) extra->push_back(ref);
  } else {
    u32 extra_count = r.Read<u32>();
    if (extra_count > 4096) {
      r.ok = false;
      return;
    }
    for (u32 i = 0; i < extra_count && r.ok; ++i) {
      i32 ref = r.Read<i32>();
      if (extra) extra->push_back(ref);
    }
  }
  r.Skip(4);  // controller
}

// 4.x bounding volume: a type-tagged union (union type recurses).
void SkipBoundingVolume(Reader& r, int depth = 0) {
  if (depth > 4) {
    r.ok = false;
    return;
  }
  u32 type = r.Read<u32>();
  switch (type) {
    case 0: r.Skip(12 + 4); break;       // sphere: center, radius
    case 1: r.Skip(12 + 36 + 12); break;  // box: center, axes, extents
    case 2: r.Skip(12 + 12 + 4 + 4); break;  // capsule
    case 4: {                             // union
      u32 count = r.Read<u32>();
      if (count > 64) {
        r.ok = false;
        return;
      }
      for (u32 i = 0; i < count && r.ok; ++i) SkipBoundingVolume(r, depth + 1);
      break;
    }
    case 5: r.Skip(16 + 12); break;  // half space: plane, center
    default: r.ok = false; break;
  }
}

AvObjectPrefix ReadAvObject(Reader& r, u32 version) {
  AvObjectPrefix out;
  ReadObjectNet(r, version, &out.name, &out.extra);
  u16 flags = r.Read<u16>();
  out.hidden = (flags & 1) != 0;
  for (f32& v : out.local.t) v = r.Read<f32>();
  for (f32& v : out.local.r) v = r.Read<f32>();
  out.local.s = r.Read<f32>();
  if (version <= kV4_0_0_2) r.Skip(12);  // velocity
  u32 property_count = r.Read<u32>();
  if (property_count > 4096) {
    r.ok = false;
    return out;
  }
  for (u32 i = 0; i < property_count && r.ok; ++i) out.properties.push_back(r.Read<i32>());
  if (version <= kV4_0_0_2) {
    if (ReadBool(r, version)) SkipBoundingVolume(r);
  } else {
    r.Skip(4);  // collision object ref
  }
  return out;
}

// NiProperty has no fields beyond NiObjectNET.
void SkipProperty(Reader& r, u32 version) { ReadObjectNet(r, version, nullptr, nullptr); }

// TexDesc: source ref, clamp/filter/uv set, PS2 shorts on 10.x, optional
// texture transform.
i32 ReadTexDesc(Reader& r, u32 version) {
  i32 source = r.Read<i32>();
  r.Skip(12);  // clamp mode, filter mode, uv set
  if (version < kV20_0_0_3) r.Skip(4);  // PS2 L/K shorts (until 10.4.0.1)
  if (version <= kV4_0_0_2) r.Skip(2);  // unknown short (until 4.1.0.12)
  if (version >= kV10_1_0_0) {
    if (r.Read<u8>()) r.Skip(8 + 8 + 4 + 4 + 8);  // texture transform
  }
  return source;
}

// KeyGroup<float>: count, key type, keys. Sizes: linear 8, quadratic 16, TBC 20.
void SkipFloatKeyGroup(Reader& r) {
  u32 count = r.Read<u32>();
  if (count == 0) return;
  u32 interpolation = r.Read<u32>();
  size_t per = interpolation == 1 ? 8 : interpolation == 2 ? 16 : interpolation == 3 ? 20 : 0;
  if (per == 0) {
    r.ok = false;
    return;
  }
  r.Skip(per * count);
}

void SkipKeyGroup(Reader& r, size_t linear, size_t quadratic, size_t tbc) {
  u32 count = r.Read<u32>();
  if (count == 0) return;
  u32 interpolation = r.Read<u32>();
  size_t per = interpolation == 1 ? linear : interpolation == 2 ? quadratic
              : interpolation == 3 ? tbc : 0;
  if (per == 0) {
    r.ok = false;
    return;
  }
  r.Skip(per * count);
}

// NiTimeController base: next, flags, frequency, phase, start, stop, target.
void SkipController(Reader& r) { r.Skip(4 + 2 + 4 * 4 + 4); }

// bhkWorldObject: shape ref, havok filter, world object cinfo.
i32 ReadBhkWorldObject(Reader& r) {
  i32 shape = r.Read<i32>();
  r.Skip(4);            // havok filter
  r.Skip(4 + 1 + 3 + 12);  // world object cinfo
  return shape;
}

void SkipConstraintBase(Reader& r) {
  u32 entities = r.Read<u32>();
  if (entities != 2) {
    r.ok = false;
    return;
  }
  r.Skip(4 + 4 + 4);  // entity a, entity b, priority
}

// NiGeometryData, 4.0.0.2 layout: u32 bools, explicit uv set count, no data
// flags, no tangents, no consistency flags.
bool ReadGeometryDataV4(Reader& r, GbGeometry* out) {
  u32 vertex_count = r.Read<u16>();
  bool has_vertices = r.Read<u32>() != 0;
  if (!r.ok || vertex_count == 0 || !has_vertices) return false;
  out->vertices.resize(vertex_count);
  const u8* positions = r.Bytes(12 * vertex_count);
  if (!positions) return false;
  for (u32 i = 0; i < vertex_count; ++i) {
    std::memcpy(out->vertices[i].position, positions + 12 * i, 12);
  }
  if (r.Read<u32>() != 0) {  // has normals
    const u8* normals = r.Bytes(12 * vertex_count);
    if (!normals) return false;
    for (u32 i = 0; i < vertex_count; ++i) {
      std::memcpy(out->vertices[i].normal, normals + 12 * i, 12);
    }
  }
  r.Skip(16);  // bounding sphere
  if (r.Read<u32>() != 0) {  // has vertex colors
    const u8* colors = r.Bytes(16 * vertex_count);
    if (!colors) return false;
    for (u32 i = 0; i < vertex_count; ++i) {
      f32 c[4];
      std::memcpy(c, colors + 16 * i, 16);
      auto pack = [](f32 v) { return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
      out->vertices[i].color = pack(c[0]) | pack(c[1]) << 8 | pack(c[2]) << 16 | pack(c[3]) << 24;
    }
  }
  u32 uv_sets = r.Read<u16>();
  bool has_uv = r.Read<u32>() != 0;
  if (has_uv && uv_sets > 0) {
    const u8* uvs = r.Bytes(8 * vertex_count);  // first set only
    if (!uvs) return false;
    for (u32 i = 0; i < vertex_count; ++i) {
      std::memcpy(out->vertices[i].uv, uvs + 8 * i, 8);
    }
    if (uv_sets > 1) r.Skip(8 * static_cast<size_t>(vertex_count) * (uv_sets - 1));
  }
  return r.ok;
}

// NiGeometryData subset shared by NiTriShapeData/NiTriStripsData.
bool ReadGeometryData(Reader& r, u32 version, GbGeometry* out) {
  if (version <= kV4_0_0_2) return ReadGeometryDataV4(r, out);
  if (version >= kV10_1_0_114) r.Skip(4);  // group id
  u32 vertex_count = r.Read<u16>();
  if (version >= kV10_1_0_0) r.Skip(2);  // keep/compress flags
  bool has_vertices = r.Read<u8>() != 0;
  if (!r.ok || vertex_count == 0 || !has_vertices) return false;

  out->vertices.resize(vertex_count);
  const u8* positions = r.Bytes(12 * vertex_count);
  if (!positions) return false;
  for (u32 i = 0; i < vertex_count; ++i) {
    std::memcpy(out->vertices[i].position, positions + 12 * i, 12);
  }

  u16 data_flags = r.Read<u16>();
  bool has_normals = r.Read<u8>() != 0;
  if (has_normals) {
    const u8* normals = r.Bytes(12 * vertex_count);
    if (!normals) return false;
    for (u32 i = 0; i < vertex_count; ++i) {
      std::memcpy(out->vertices[i].normal, normals + 12 * i, 12);
    }
    if (version >= kV10_1_0_0 && (data_flags & 0xF000)) {
      const u8* tangents = r.Bytes(12 * vertex_count);
      r.Skip(12 * vertex_count);  // bitangents
      if (!tangents) return false;
      for (u32 i = 0; i < vertex_count; ++i) {
        std::memcpy(out->vertices[i].tangent, tangents + 12 * i, 12);
        out->vertices[i].tangent[3] = 1;
      }
    }
  }
  r.Skip(16);  // bounding sphere
  bool has_colors = r.Read<u8>() != 0;
  if (has_colors) {
    const u8* colors = r.Bytes(16 * vertex_count);
    if (!colors) return false;
    for (u32 i = 0; i < vertex_count; ++i) {
      f32 c[4];
      std::memcpy(c, colors + 16 * i, 16);
      auto pack = [](f32 v) { return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
      out->vertices[i].color = pack(c[0]) | pack(c[1]) << 8 | pack(c[2]) << 16 | pack(c[3]) << 24;
    }
  }
  u32 uv_sets = data_flags & 0x3f;
  if (uv_sets > 0) {
    const u8* uvs = r.Bytes(8 * vertex_count);  // first set only
    if (!uvs) return false;
    for (u32 i = 0; i < vertex_count; ++i) {
      std::memcpy(out->vertices[i].uv, uvs + 8 * i, 8);
    }
    if (uv_sets > 1) r.Skip(8 * static_cast<size_t>(vertex_count) * (uv_sets - 1));
  }
  r.Skip(2);  // consistency flags
  if (version >= kV20_0_0_4) r.Skip(4);  // additional data ref
  return r.ok;
}

// Parses one block. Returns false on a structural error; unknown block types
// also land here because the stream cannot be advanced past them.
bool ParseBlock(Reader& r, GbScene& scene, u32 index, const std::string& type) {
  const u32 version = scene.version;

  if (type == "NiNode" || type == "NiBillboardNode" || type == "BSFadeNode" ||
      type == "RootCollisionNode" || type == "AvoidNode" || type == "NiBSAnimationNode" ||
      type == "NiBSParticleNode") {
    AvObjectPrefix av = ReadAvObject(r, version);
    GbNode node;
    node.local = av.local;
    // Collision-only subtrees never draw.
    node.hidden = av.hidden || type == "RootCollisionNode" || type == "AvoidNode";
    node.properties = std::move(av.properties);
    u32 child_count = r.Read<u32>();
    if (!r.ok || child_count > 65536) return false;
    node.children.reserve(child_count);
    for (u32 i = 0; i < child_count; ++i) node.children.push_back(r.Read<i32>());
    u32 effect_count = r.Read<u32>();
    if (effect_count > 4096) return false;
    r.Skip(4 * effect_count);
    if (type == "NiBillboardNode" && version > kV4_0_0_2) r.Skip(2);  // billboard mode
    if (r.ok) scene.nodes.emplace(index, std::move(node));
    return r.ok;
  }
  if (type == "NiTriShape" || type == "NiTriStrips") {
    AvObjectPrefix av = ReadAvObject(r, version);
    GbShape shape;
    shape.local = av.local;
    shape.name = std::move(av.name);
    shape.extra = std::move(av.extra);
    shape.properties = std::move(av.properties);
    shape.hidden = av.hidden;
    shape.data = r.Read<i32>();
    shape.skin = r.Read<i32>();
    if (version > kV4_0_0_2 && r.Read<u8>()) {  // has shader (10.x+)
      ReadSizedString(r);
      r.Skip(4);
    }
    if (r.ok) scene.shapes.emplace(index, std::move(shape));
    return r.ok;
  }
  // Particle emitters: geometry-shaped blocks whose data we do not render yet;
  // parsed so the walk continues (the shape is not registered).
  if (type == "NiAutoNormalParticles" || type == "NiRotatingParticles") {
    ReadAvObject(r, version);
    r.Skip(8);  // data ref, skin ref
    return r.ok;
  }
  if (type == "NiAutoNormalParticlesData" || type == "NiRotatingParticlesData") {
    GbGeometry ignored;
    if (!ReadGeometryDataV4(r, &ignored) && !r.ok) return false;
    u32 vertex_count = static_cast<u32>(ignored.vertices.size());
    r.Skip(2 + 4 + 2);  // num particles, radius, num active
    if (r.Read<u32>() != 0) r.Skip(4 * vertex_count);  // sizes
    if (type == "NiRotatingParticlesData") {
      if (r.Read<u32>() != 0) r.Skip(16 * vertex_count);  // rotations
    }
    return r.ok;
  }
  if (type == "NiTriShapeData") {
    GbGeometry geometry;
    if (!ReadGeometryData(r, version, &geometry)) return false;
    u32 triangle_count = r.Read<u16>();
    r.Skip(4);  // num triangle points
    bool has_triangles = version > kV10_0_1_2 ? r.Read<u8>() != 0 : true;
    if (!r.ok) return false;
    if (has_triangles) {
      const u8* tris = r.Bytes(6 * triangle_count);
      if (!tris) return false;
      geometry.indices.resize(triangle_count * 3);
      for (u32 i = 0; i < triangle_count * 3u; ++i) {
        u16 v;
        std::memcpy(&v, tris + i * 2, 2);
        if (v >= geometry.vertices.size()) return false;
        geometry.indices[i] = v;
      }
    }
    u32 match_groups = r.Read<u16>();
    for (u32 i = 0; i < match_groups && r.ok; ++i) {
      u32 count = r.Read<u16>();
      r.Skip(2 * count);
    }
    if (r.ok) scene.geometry.emplace(index, std::move(geometry));
    return r.ok;
  }
  if (type == "NiTriStripsData") {
    GbGeometry geometry;
    if (!ReadGeometryData(r, version, &geometry)) return false;
    r.Skip(2);  // num triangles
    u32 strip_count = r.Read<u16>();
    base::Vector<u16> lengths(strip_count);
    for (u16& l : lengths) l = r.Read<u16>();
    bool has_points = version > kV10_0_1_2 ? r.Read<u8>() != 0 : true;
    if (!r.ok) return false;
    if (has_points) {
      for (u16 length : lengths) {
        const u8* points = r.Bytes(2 * static_cast<size_t>(length));
        if (!points) return false;
        for (u32 i = 0; i + 2 < length; ++i) {
          u16 a, b, c;
          std::memcpy(&a, points + i * 2, 2);
          std::memcpy(&b, points + i * 2 + 2, 2);
          std::memcpy(&c, points + i * 2 + 4, 2);
          if (a == b || b == c || a == c) continue;
          if (a >= geometry.vertices.size() || b >= geometry.vertices.size() ||
              c >= geometry.vertices.size()) {
            return false;
          }
          // Odd strip triangles flip winding.
          if (i & 1) std::swap(b, c);
          geometry.indices.push_back(a);
          geometry.indices.push_back(b);
          geometry.indices.push_back(c);
        }
      }
    }
    if (r.ok) scene.geometry.emplace(index, std::move(geometry));
    return r.ok;
  }
  if (type == "NiTexturingProperty") {
    SkipProperty(r, version);
    if (version <= kV10_0_1_2) r.Skip(2);  // property flags (until 10.0.1.2)
    r.Skip(4);  // apply mode
    u32 texture_count = r.Read<u32>();
    if (!r.ok || texture_count > 32) return false;
    GbTexturing tex;
    // base, dark, detail, gloss, glow
    for (u32 slot = 0; slot < 5 && slot < texture_count && r.ok; ++slot) {
      if (!ReadBool(r, version)) continue;
      i32 source = ReadTexDesc(r, version);
      if (slot == 0) tex.base_source = source;
      if (slot == 4) tex.glow_source = source;
    }
    if (texture_count > 5 && ReadBool(r, version)) {  // bump map
      ReadTexDesc(r, version);
      r.Skip(4 + 4 + 16);  // luma scale/offset, 2x2 matrix
    }
    for (u32 slot = 6; slot < texture_count && slot < 10 && r.ok; ++slot) {  // decals
      if (ReadBool(r, version)) ReadTexDesc(r, version);
    }
    if (version > kV4_0_0_2) {  // shader textures (10.x+)
      u32 shader_textures = r.Read<u32>();
      if (shader_textures > 256) return false;
      for (u32 i = 0; i < shader_textures && r.ok; ++i) {
        if (r.Read<u8>()) {
          ReadTexDesc(r, version);
          r.Skip(4);  // map id
        }
      }
    }
    if (r.ok) scene.texturing.emplace(index, tex);
    return r.ok;
  }
  if (type == "NiSourceTexture") {
    std::string name;
    ReadObjectNet(r, version, &name, nullptr);
    bool external = r.Read<u8>() != 0;
    std::string file;
    if (version >= kV10_1_0_0) {
      // The (original) file name is stored either way, plus a pixel data ref.
      file = ReadSizedString(r);
      r.Skip(4);
    } else if (external) {
      file = ReadSizedString(r);
    } else {
      r.Skip(1 + 4);  // use internal, pixel data ref
    }
    r.Skip(12 + 1);  // format prefs, is static
    if (version >= kV10_1_0_103) r.Skip(1);  // direct render
    if (r.ok) scene.textures.emplace(index, std::move(file));
    return r.ok;
  }
  if (type == "NiMaterialProperty") {
    SkipProperty(r, version);
    if (version <= kV10_0_1_2) r.Skip(2);  // property flags (until 10.0.1.2)
    GbMaterial material;
    r.Skip(24);  // ambient, diffuse
    r.Skip(12);  // specular
    for (f32& v : material.emissive) v = r.Read<f32>();
    material.glossiness = r.Read<f32>();
    material.alpha = r.Read<f32>();
    if (r.ok) scene.materials.emplace(index, material);
    return r.ok;
  }
  if (type == "NiAlphaProperty") {
    SkipProperty(r, version);
    GbAlpha alpha;
    alpha.flags = r.Read<u16>();
    alpha.threshold = r.Read<u8>();
    if (r.ok) scene.alphas.emplace(index, alpha);
    return r.ok;
  }
  if (type == "NiStencilProperty") {
    SkipProperty(r, version);
    if (version <= kV10_0_1_2) r.Skip(2);  // property flags (until 10.0.1.2)
    r.Skip(1 + 4 * 6);  // enabled, function, ref, mask, fail/zfail/pass
    u32 draw_mode = r.Read<u32>();
    if (r.ok) scene.stencil_draw_mode.emplace(index, draw_mode);
    return r.ok;
  }
  if (type == "NiSpecularProperty" || type == "NiShadeProperty" || type == "NiDitherProperty" ||
      type == "NiWireframeProperty") {
    SkipProperty(r, version);
    r.Skip(2);
    return r.ok;
  }
  if (type == "NiVertexColorProperty") {
    SkipProperty(r, version);
    r.Skip(2 + 4 + 4);
    return r.ok;
  }
  if (type == "NiZBufferProperty") {
    SkipProperty(r, version);
    r.Skip(2);
    if (version > kV4_0_0_2) r.Skip(4);  // z compare function (4.1.0.12+)
    return r.ok;
  }
  if (type == "NiFogProperty") {
    SkipProperty(r, version);
    r.Skip(2 + 4 + 12);
    return r.ok;
  }
  // ---- extra data ----
  if (type == "NiBinaryExtraData") {
    std::string name = ReadSizedString(r);
    u32 size = r.Read<u32>();
    const u8* bytes = r.Bytes(size);
    if (!r.ok) return false;
    if (name.starts_with("Tangent space")) {
      scene.tangents.emplace(index, GbTangents{bytes, size});
    }
    return true;
  }
  if (type == "NiStringExtraData") {
    if (version <= kV4_0_0_2) {
      r.Skip(4 + 4);  // next extra data ref, bytes remaining
    } else {
      ReadSizedString(r);  // name
    }
    ReadSizedString(r);
    return r.ok;
  }
  if (type == "NiVertWeightsExtraData") {  // 4.x only
    r.Skip(4 + 4 + 4);  // next ref, bytes remaining, num bytes
    u32 count = r.Read<u16>();
    r.Skip(4 * count);
    return r.ok;
  }
  if (type == "BSXFlags" || type == "NiIntegerExtraData") {
    ReadSizedString(r);
    r.Skip(4);
    return r.ok;
  }
  if (type == "NiFloatExtraData") {
    ReadSizedString(r);
    r.Skip(4);
    return r.ok;
  }
  if (type == "NiIntegersExtraData") {
    ReadSizedString(r);
    u32 count = r.Read<u32>();
    if (count > 65536) return false;
    r.Skip(4 * count);
    return r.ok;
  }
  if (type == "NiTextKeyExtraData") {
    if (version <= kV4_0_0_2) {
      r.Skip(4 + 4);  // next extra data ref, bytes remaining
    } else {
      ReadSizedString(r);
    }
    u32 count = r.Read<u32>();
    if (count > 4096) return false;
    for (u32 i = 0; i < count && r.ok; ++i) {
      r.Skip(4);
      ReadSizedString(r);
    }
    return r.ok;
  }
  if (type == "BSFurnitureMarker") {
    ReadSizedString(r);
    u32 count = r.Read<u32>();
    if (count > 256) return false;
    r.Skip(16 * count);
    return r.ok;
  }
  if (type == "BSBound") {
    ReadSizedString(r);
    r.Skip(24);
    return r.ok;
  }
  // ---- havok collision (structural skip) ----
  if (type == "bhkCollisionObject" || type == "bhkSPCollisionObject") {
    r.Skip(4 + 2 + 4);  // target, flags, body
    return r.ok;
  }
  if (type == "bhkRigidBody" || type == "bhkRigidBodyT") {
    ReadBhkWorldObject(r);
    r.Skip(1 + 1 + 2);  // entity cinfo
    // bhkRigidBodyCInfo550_660
    r.Skip(4 + 4 + 4 + 1 + 1 + 2 + 4);
    r.Skip(16 * 4 + 48 + 16 + 4 * 5);  // trans/rot/velocities, inertia, center, scalars
    r.Skip(4 * 3);                     // max velocities, penetration depth
    r.Skip(1 + 1 + 1 + 1 + 12);        // motion/deactivator/solver/quality, unused
    u32 constraints = r.Read<u32>();
    if (constraints > 1024) return false;
    r.Skip(4 * constraints + 4);  // constraint refs, body flags
    return r.ok;
  }
  if (type == "bhkNiTriStripsShape") {
    r.Skip(4 + 4 + 20 + 4);  // material, radius, unused, grow by
    r.Skip(16);              // scale
    u32 strips = r.Read<u32>();
    if (strips > 4096) return false;
    r.Skip(4 * strips);
    u32 filters = r.Read<u32>();
    if (filters > 4096) return false;
    r.Skip(4 * filters);
    return r.ok;
  }
  if (type == "bhkMoppBvTreeShape") {
    r.Skip(4 + 12 + 4);  // shape ref, unused, scale
    u32 mopp_size = r.Read<u32>();
    r.Skip(16);  // offset
    r.Skip(mopp_size);
    return r.ok;
  }
  if (type == "bhkConvexVerticesShape") {
    r.Skip(4 + 4 + 24);  // material, radius, cinfo properties
    u32 vertex_count = r.Read<u32>();
    if (vertex_count > 65536) return false;
    r.Skip(16 * vertex_count);
    u32 normal_count = r.Read<u32>();
    if (normal_count > 65536) return false;
    r.Skip(16 * normal_count);
    return r.ok;
  }
  if (type == "bhkBoxShape") {
    r.Skip(4 + 4 + 8 + 12 + 4);
    return r.ok;
  }
  if (type == "bhkCapsuleShape") {
    r.Skip(4 + 4 + 8 + 12 + 4 + 12 + 4);
    return r.ok;
  }
  if (type == "bhkSphereShape") {
    r.Skip(4 + 4);
    return r.ok;
  }
  if (type == "bhkListShape") {
    u32 children = r.Read<u32>();
    if (children > 4096) return false;
    r.Skip(4 * children);
    r.Skip(4 + 24);  // material, cinfo properties
    u32 filters = r.Read<u32>();
    if (filters > 4096) return false;
    r.Skip(4 * filters);
    return r.ok;
  }
  if (type == "bhkTransformShape" || type == "bhkConvexTransformShape") {
    r.Skip(4 + 4 + 4 + 8 + 64);  // shape, material, radius, unused, matrix
    return r.ok;
  }
  if (type == "bhkPackedNiTriStripsShape") {
    u32 sub_shapes = r.Read<u16>();
    if (sub_shapes > 4096) return false;
    r.Skip(12 * sub_shapes);
    r.Skip(4 + 4 + 4 + 4 + 16 + 4 + 16 + 4);  // user data..scale copy, data ref
    return r.ok;
  }
  if (type == "hkPackedNiTriStripsData") {
    u32 triangles = r.Read<u32>();
    if (triangles > (1u << 20)) return false;
    r.Skip(20 * static_cast<size_t>(triangles));
    u32 vertices = r.Read<u32>();
    if (vertices > (1u << 20)) return false;
    r.Skip(12 * static_cast<size_t>(vertices));
    return r.ok;
  }
  if (type == "bhkSimpleShapePhantom") {
    ReadBhkWorldObject(r);
    r.Skip(8 + 64);
    return r.ok;
  }
  if (type == "bhkLimitedHingeConstraint") {
    SkipConstraintBase(r);
    r.Skip(7 * 16 + 3 * 4);
    return r.ok;
  }
  if (type == "bhkRagdollConstraint") {
    SkipConstraintBase(r);
    r.Skip(6 * 16 + 6 * 4);
    return r.ok;
  }
  if (type == "bhkHingeConstraint") {
    SkipConstraintBase(r);
    r.Skip(5 * 16);
    return r.ok;
  }
  if (type == "bhkStiffSpringConstraint") {
    SkipConstraintBase(r);
    r.Skip(36);
    return r.ok;
  }
  if (type == "bhkPrismaticConstraint") {
    SkipConstraintBase(r);
    r.Skip(8 * 16 + 3 * 4);
    return r.ok;
  }
  // ---- 4.x animation, particles, effects, skinning (structural skip) ----
  if (type == "NiKeyframeController" || type == "NiVisController" || type == "NiUVController" ||
      type == "NiPathController" || type == "NiGeomMorpherController" ||
      type == "NiLookAtController" || type == "NiRollController") {
    SkipController(r);
    if (type == "NiUVController") r.Skip(2);  // unknown short
    if (type == "NiPathController") {
      // bank dir, max bank angle, smoothing, follow axis, then two data refs.
      r.Skip(4 + 4 + 4 + 2 + 4 + 4);
      return r.ok;
    }
    if (type == "NiRollController") {
      r.Skip(4);  // float data ref
      return r.ok;
    }
    r.Skip(4);  // data ref
    if (type == "NiGeomMorpherController") r.Skip(1);  // always update
    return r.ok;
  }
  if (type == "NiKeyframeData") {
    u32 rotations = r.Read<u32>();
    if (rotations) {
      u32 rotation_type = r.Read<u32>();
      if (rotation_type == 4) {
        r.Skip(4);  // axis order / unknown float (until 10.1)
        for (int k = 0; k < 3; ++k) SkipFloatKeyGroup(r);
      } else {
        size_t per = rotation_type == 1 || rotation_type == 2 ? 20
                     : rotation_type == 3 ? 32 : 0;
        if (per == 0) return false;
        r.Skip(per * rotations);
      }
    }
    SkipKeyGroup(r, 16, 40, 28);  // translations
    SkipFloatKeyGroup(r);         // scales
    return r.ok;
  }
  if (type == "NiUVData") {
    for (int k = 0; k < 4; ++k) SkipFloatKeyGroup(r);
    return r.ok;
  }
  if (type == "NiVisData") {
    u32 count = r.Read<u32>();
    if (count > 65536) return false;
    r.Skip(5 * count);  // time f32 + value u8
    return r.ok;
  }
  if (type == "NiMorphData") {
    u32 morphs = r.Read<u32>();
    u32 vertices = r.Read<u32>();
    r.Skip(1);  // relative targets
    if (!r.ok || morphs > 1024 || vertices > (1u << 20)) return false;
    for (u32 m = 0; m < morphs && r.ok; ++m) {
      u32 keys = r.Read<u32>();
      u32 interpolation = r.Read<u32>();
      size_t per = interpolation == 1 ? 8 : interpolation == 2 ? 16 : interpolation == 3 ? 20 : 0;
      if (keys > 0 && per == 0) return false;
      r.Skip(per * keys);
      r.Skip(12 * static_cast<size_t>(vertices));
    }
    return r.ok;
  }
  if (type == "NiParticleSystemController" || type == "NiBSPArrayController") {
    SkipController(r);
    r.Skip(6 * 4);       // speed/randoms, directions, angles
    r.Skip(12 + 16);     // unknown normal, unknown color
    r.Skip(4 + 4 + 4);   // size, emit start, emit stop
    r.Skip(1);           // unknown byte (4.0.0.2+)
    r.Skip(4 + 4 + 4);   // emit rate, lifetime, lifetime random
    r.Skip(2 + 12 + 4);  // emit flags, start random, emitter ref
    r.Skip(2 + 4 + 4 + 4 + 2);  // unknown shorts/floats/ints
    u32 particles = r.Read<u16>();
    r.Skip(2);  // num valid
    r.Skip(40 * static_cast<size_t>(particles));
    r.Skip(4 + 4 + 4 + 1);  // unknown ref, modifier ref, unknown ref, trailer
    return r.ok;
  }
  if (type == "NiGravity") {
    r.Skip(8 + 4 + 4 + 4 + 12 + 12);  // modifier base, decay, force, type, position, direction
    return r.ok;
  }
  if (type == "NiParticleGrowFade") {
    r.Skip(8 + 4 + 4);
    return r.ok;
  }
  if (type == "NiParticleColorModifier") {
    r.Skip(8 + 4);
    return r.ok;
  }
  if (type == "NiParticleRotation") {
    r.Skip(8 + 1 + 12 + 4);
    return r.ok;
  }
  if (type == "NiTextureEffect") {
    ReadAvObject(r, version);
    // NiDynamicEffect (4.x): affected node list pointers (raw memory values).
    if (version <= kV4_0_0_2) {
      u32 affected = r.Read<u32>();
      if (affected > 4096) return false;
      r.Skip(4 * affected);
    }
    r.Skip(36 + 12);          // model projection matrix + translation
    r.Skip(4 + 4 + 4 + 4);    // filtering, clamping, type, coordinate generation
    r.Skip(4);                // source texture ref
    r.Skip(1);                // clipping plane enable (a byte in every version)
    r.Skip(12 + 4);           // plane normal + constant
    if (version < kV20_0_0_3) r.Skip(4);   // PS2 L/K
    if (version <= kV4_0_0_2) r.Skip(2);   // unknown short
    return r.ok;
  }
  if (type == "NiSkinInstance") {
    r.Skip(4);  // data ref
    if (version > kV4_0_0_2) r.Skip(4);  // skin partition ref (10.1+)
    r.Skip(4);  // skeleton root ref
    u32 bones = r.Read<u32>();
    if (bones > 1024) return false;
    r.Skip(4 * bones);
    return r.ok;
  }
  if (type == "NiSkinData" && version <= kV4_0_0_2) {
    r.Skip(52);  // overall transform
    u32 bones = r.Read<u32>();
    r.Skip(4);   // skin partition ref (4.0.0.2 .. 10.1)
    if (!r.ok || bones > 1024) return false;
    for (u32 b = 0; b < bones && r.ok; ++b) {
      r.Skip(52 + 16);  // bone transform, bounding sphere
      u32 weights = r.Read<u16>();
      r.Skip(6 * weights);
    }
    return r.ok;
  }
  // ---- animation (structural skip so animated statics still render) ----
  if (type == "NiTransformController" || type == "NiAlphaController") {
    SkipController(r);
    r.Skip(4);  // interpolator (10.x) / float data ref (4.x)
    return r.ok;
  }
  if (type == "NiMaterialColorController") {
    SkipController(r);
    r.Skip(4);  // interpolator (10.x) / color data ref (4.x)
    if (version > kV4_0_0_2) r.Skip(2);  // target color
    return r.ok;
  }
  if (type == "NiTextureTransformController") {
    SkipController(r);
    r.Skip(4 + 1 + 4 + 4);  // interpolator, shader map, texture slot, operation
    return r.ok;
  }
  if (type == "NiFlipController") {
    SkipController(r);
    r.Skip(4 + 4);  // interpolator, texture slot
    u32 count = r.Read<u32>();
    if (count > 4096) return false;
    r.Skip(4 * count);
    return r.ok;
  }
  if (type == "NiMultiTargetTransformController") {
    SkipController(r);
    u32 count = r.Read<u16>();
    r.Skip(4 * count);
    return r.ok;
  }
  if (type == "NiControllerManager") {
    SkipController(r);
    r.Skip(1);  // cumulative
    u32 count = r.Read<u32>();
    if (count > 4096) return false;
    r.Skip(4 * count + 4);  // sequences, object palette
    return r.ok;
  }
  if (type == "NiControllerSequence") {
    ReadSizedString(r);  // name
    u32 blocks = r.Read<u32>();
    if (blocks > 4096) return false;
    r.Skip(4);  // array grow by
    for (u32 i = 0; i < blocks && r.ok; ++i) {
      r.Skip(4 + 4 + 1);  // interpolator, controller, priority
      if (version <= kV10_1_0_106) {
        for (int k = 0; k < 5; ++k) ReadSizedString(r);
      } else {
        r.Skip(4 + 4 * 5);  // string palette + 5 offsets
      }
    }
    r.Skip(4 + 4 + 4 + 4);  // weight, text keys, cycle type, frequency
    if (version <= 0x0A040001) r.Skip(4);  // phase (until 10.4.0.1)
    r.Skip(4 + 4);  // start, stop
    if (version == kV10_1_0_106) r.Skip(1);  // play backwards
    r.Skip(4);  // manager
    ReadSizedString(r);  // accum root name
    r.Skip(4);  // string palette
    return r.ok;
  }
  if (type == "NiStringPalette") {
    ReadSizedString(r);
    r.Skip(4);
    return r.ok;
  }
  if (type == "NiDefaultAVObjectPalette") {
    r.Skip(4);
    u32 count = r.Read<u32>();
    if (count > 65536) return false;
    for (u32 i = 0; i < count && r.ok; ++i) {
      ReadSizedString(r);
      r.Skip(4);
    }
    return r.ok;
  }
  if (type == "NiTransformInterpolator") {
    r.Skip(12 + 16 + 4 + 4);  // translation, rotation, scale, data ref
    return r.ok;
  }
  if (type == "NiTransformData") {
    u32 rotations = r.Read<u32>();
    if (rotations) {
      u32 rotation_type = r.Read<u32>();
      if (rotation_type == 4) {
        for (int k = 0; k < 3; ++k) SkipFloatKeyGroup(r);
      } else {
        size_t per = rotation_type == 1 || rotation_type == 2 ? 20
                     : rotation_type == 3 ? 32 : 0;
        if (per == 0) return false;
        r.Skip(per * rotations);
      }
    }
    SkipKeyGroup(r, 16, 40, 28);  // translations (Vector3)
    SkipFloatKeyGroup(r);         // scales
    return r.ok;
  }
  if (type == "NiFloatInterpolator") {
    r.Skip(4 + 4);
    return r.ok;
  }
  if (type == "NiFloatData") {
    SkipFloatKeyGroup(r);
    return r.ok;
  }
  if (type == "NiBoolInterpolator") {
    r.Skip(1 + 4);
    return r.ok;
  }
  if (type == "NiBoolData") {
    SkipKeyGroup(r, 5, 13, 17);
    return r.ok;
  }
  if (type == "NiPoint3Interpolator") {
    r.Skip(12 + 4);
    return r.ok;
  }
  if (type == "NiPosData") {
    SkipKeyGroup(r, 16, 40, 28);
    return r.ok;
  }
  if (type == "NiColorData") {
    SkipKeyGroup(r, 20, 52, 32);
    return r.ok;
  }
  if (type == "NiBlendFloatInterpolator" || type == "NiBlendBoolInterpolator" ||
      type == "NiBlendPoint3Interpolator" || type == "NiBlendTransformInterpolator") {
    // NiBlendInterpolator, 10.1.0.112+ layout.
    u8 flags = r.Read<u8>();
    u8 array_size = r.Read<u8>();
    r.Skip(4);  // weight threshold
    if (!(flags & 1)) {
      r.Skip(4 + 4 * 4);  // counts/indices, times/weights
      r.Skip(10 * static_cast<size_t>(array_size));  // interp blend items
    }
    if (type == "NiBlendFloatInterpolator") r.Skip(4);
    if (type == "NiBlendBoolInterpolator") r.Skip(1);
    if (type == "NiBlendPoint3Interpolator") r.Skip(12);
    return r.ok;
  }
  // ---- lights ----
  if (type == "NiDirectionalLight" || type == "NiAmbientLight" || type == "NiPointLight" ||
      type == "NiSpotLight") {
    ReadAvObject(r, version);
    if (version >= kV10_1_0_106) r.Skip(1);  // switch state
    u32 affected = r.Read<u32>();
    if (affected > 4096) return false;
    r.Skip(4 * affected);
    r.Skip(4 + 36);  // dimmer, ambient/diffuse/specular
    if (type == "NiPointLight" || type == "NiSpotLight") r.Skip(12);  // attenuation
    if (type == "NiSpotLight") r.Skip(4 + 4);  // cutoff angle, exponent
    return r.ok;
  }
  return false;  // unknown block type: the walk cannot continue
}

bool ParseScene(ByteSpan data, std::string_view source_path, GbScene* scene) {
  std::string_view text(reinterpret_cast<const char*>(data.data()),
                        std::min<size_t>(data.size(), 80));
  size_t newline = text.find('\n');
  if (newline == std::string_view::npos) return false;

  Reader r{data, newline + 1};
  scene->version = r.Read<u32>();

  // NetImmerse 4.x (Morrowind): no type table, no user version, no separators;
  // every block is inline-typed with a sized string.
  if (scene->version <= kV4_0_0_2) {
    u32 block_count = r.Read<u32>();
    if (!r.ok || block_count > 65536) return false;
    scene->block_types.reserve(block_count);
    for (u32 i = 0; i < block_count; ++i) {
      std::string type = ReadSizedString(r);
      if (!r.ok) return false;
      scene->block_types.push_back(type);
      if (!ParseBlock(r, *scene, i, type) || !r.ok) {
        RX_DEBUG("netimmerse nif: stopped at block {} ({}) in {}", i, type, source_path);
        return false;
      }
    }
    u32 root_count = r.Read<u32>();
    if (!r.ok || root_count > 4096) return false;
    for (u32 i = 0; i < root_count; ++i) scene->roots.push_back(r.Read<i32>());
    return r.ok;
  }

  if (scene->version >= kV20_0_0_3) r.Skip(1);  // endian
  u32 user_version = 0;
  if (scene->version > kV10_0_1_2) user_version = r.Read<u32>();  // since 10.0.1.8
  u32 block_count = r.Read<u32>();
  if (!r.ok || block_count > 65536) return false;
  // Bethesda export header: 10.0.1.2 ships it despite predating User Version.
  if (user_version >= 3 || scene->version == kV10_0_1_2) {
    r.Skip(4);  // BS stream version
    for (int i = 0; i < 3; ++i) r.Skip(r.Read<u8>());  // export strings
  }
  u16 type_count = r.Read<u16>();
  if (!r.ok || type_count > 4096) return false;
  base::Vector<std::string> types;
  types.reserve(type_count);
  for (u16 i = 0; i < type_count; ++i) types.push_back(ReadSizedString(r));
  scene->block_types.reserve(block_count);
  for (u32 i = 0; i < block_count; ++i) {
    u16 type_index = r.Read<u16>() & 0x7fff;
    if (type_index >= types.size()) return false;
    scene->block_types.push_back(types[type_index]);
  }
  u32 group_count = r.Read<u32>();
  r.Skip(4 * static_cast<size_t>(group_count));
  if (!r.ok) return false;

  for (u32 i = 0; i < block_count; ++i) {
    if (scene->version < kV10_2_0_0) r.Skip(4);  // zero separator on 10.1.x
    const std::string& type = scene->block_types[i];
    if (!ParseBlock(r, *scene, i, type) || !r.ok) {
      RX_DEBUG("gamebryo nif: stopped at block {} ({}) in {}", i, type, source_path);
      return false;
    }
  }
  u32 root_count = r.Read<u32>();
  if (!r.ok || root_count > 4096) return false;
  for (u32 i = 0; i < root_count; ++i) scene->roots.push_back(r.Read<i32>());
  return r.ok;
}

// Property set effective for a shape: Gamebryo properties inherit down the
// node tree, a child's own property of the same class overrides.
struct PropertySet {
  i32 texturing = -1;
  i32 material = -1;
  i32 alpha = -1;
  i32 stencil = -1;

  void Apply(const GbScene& scene, const base::Vector<i32>& properties) {
    for (i32 property : properties) {
      if (property < 0) continue;
      u32 block = static_cast<u32>(property);
      if (scene.texturing.find(block)) texturing = property;
      else if (scene.materials.find(block)) material = property;
      else if (scene.alphas.find(block)) alpha = property;
      else if (scene.stencil_draw_mode.find(block)) stencil = property;
    }
  }
};

}  // namespace

NifConversion ConvertGamebryoNif(ByteSpan data, asset::AssetId id, std::string_view source_path) {
  NifConversion result;
  result.gamebryo = true;
  GbScene scene;
  if (!ParseScene(data, source_path, &scene)) {
    RX_DEBUG("unsupported gamebryo nif: {}", source_path);
    return result;
  }

  auto mesh = base::MakeUnique<asset::Mesh>();
  mesh->id = id;
  mesh->lods.emplace_back();
  asset::MeshLod& lod = mesh->lods[0];

  // Material per distinct (texturing, material, alpha, stencil) combination.
  base::UnorderedMap<u64, asset::AssetId> material_ids;
  auto material_for = [&](const PropertySet& props) -> asset::AssetId {
    u64 key = (static_cast<u64>(static_cast<u16>(props.texturing)) << 48) |
              (static_cast<u64>(static_cast<u16>(props.material)) << 32) |
              (static_cast<u64>(static_cast<u16>(props.alpha)) << 16) |
              static_cast<u64>(static_cast<u16>(props.stencil));
    if (asset::AssetId* known = material_ids.find(key)) return *known;

    asset::Material material;
    std::string name =
        std::string(source_path) + "#m" + std::to_string(result.materials.size());
    material.id = asset::MakeAssetId(name);
    material.metallic_factor = 0;

    // Morrowind NIFs reference .tga/.bmp sources, but the archives ship every
    // texture converted to .dds; swap the extension so the vfs lookup hits.
    auto texture_path = [&](const std::string& file) {
      std::string path = NormalizeTexturePath(file);
      if (scene.version <= 0x04000002 &&
          (path.ends_with(".tga") || path.ends_with(".bmp"))) {
        path = path.substr(0, path.size() - 4) + ".dds";
      }
      return path;
    };
    std::string diffuse, glow;
    if (const GbTexturing* tex = scene.texturing.find(static_cast<u32>(props.texturing))) {
      if (const std::string* file = scene.textures.find(static_cast<u32>(tex->base_source))) {
        diffuse = texture_path(*file);
      }
      if (const std::string* file = scene.textures.find(static_cast<u32>(tex->glow_source))) {
        glow = texture_path(*file);
      }
    }
    if (!diffuse.empty()) {
      material.base_color = asset::MakeAssetId(diffuse);
      if (std::ranges::find(result.texture_paths, diffuse) == result.texture_paths.end()) {
        result.texture_paths.push_back(diffuse);
      }
      // Oblivion has no normal-map slot in the texture set; the convention is
      // a sibling "<diffuse>_n.dds". The converter existence-checks it (the
      // gamebryo flag) and drops the binding when the file is absent.
      if (diffuse.ends_with(".dds")) {
        std::string normal = diffuse.substr(0, diffuse.size() - 4) + "_n.dds";
        material.normal = asset::MakeAssetId(normal);
        if (std::ranges::find(result.texture_paths, normal) == result.texture_paths.end()) {
          result.texture_paths.push_back(std::move(normal));
        }
      }
    }
    if (!glow.empty()) {
      material.emissive = asset::MakeAssetId(glow);
      if (std::ranges::find(result.texture_paths, glow) == result.texture_paths.end()) {
        result.texture_paths.push_back(std::move(glow));
      }
    }
    if (const GbMaterial* mat = scene.materials.find(static_cast<u32>(props.material))) {
      // Specular power to perceptual roughness, matching the 20.2.0.7 path.
      f32 gloss = std::clamp(mat->glossiness, 1.0f, 1000.0f);
      material.roughness_factor =
          std::clamp(std::pow(2.0f / (gloss + 2.0f), 0.25f), 0.2f, 1.0f);
      for (int k = 0; k < 3; ++k) material.emissive_factor[k] = mat->emissive[k];
      if (mat->alpha < 1.0f) {
        material.base_color_factor[3] = mat->alpha;
      }
    }
    if (const GbAlpha* alpha = scene.alphas.find(static_cast<u32>(props.alpha))) {
      bool blend = (alpha->flags & 1) != 0;
      bool test = (alpha->flags & (1u << 9)) != 0;
      if (test) {
        material.alpha_mode = asset::AlphaMode::kMask;
        material.alpha_cutoff = static_cast<f32>(alpha->threshold) / 255.0f;
      } else if (blend) {
        material.alpha_mode = asset::AlphaMode::kBlend;
      }
    }
    if (const u32* draw_mode = scene.stencil_draw_mode.find(static_cast<u32>(props.stencil))) {
      if (*draw_mode == 3) material.two_sided = true;  // DRAW_BOTH
    }

    result.materials.push_back(material);
    material_ids.emplace(key, material.id);
    return material.id;
  };

  // Flatten depth first from the roots, properties inheriting down the tree.
  struct StackEntry {
    u32 block;
    Transform world;
    PropertySet props;
  };
  base::Vector<StackEntry> stack;
  for (i32 root : scene.roots) {
    if (root >= 0) stack.push_back({static_cast<u32>(root), Transform{}, PropertySet{}});
  }
  base::Vector<u8> visited(scene.block_types.size());

  f32 bounds_min[3] = {1e30f, 1e30f, 1e30f};
  f32 bounds_max[3] = {-1e30f, -1e30f, -1e30f};

  while (!stack.empty()) {
    StackEntry entry = stack.back();
    stack.pop_back();
    if (entry.block >= scene.block_types.size() || visited[entry.block]) continue;
    visited[entry.block] = true;

    if (const GbNode* node = scene.nodes.find(entry.block)) {
      if (node->hidden) continue;
      Transform world = Compose(entry.world, node->local);
      PropertySet props = entry.props;
      props.Apply(scene, node->properties);
      for (i32 child : node->children) {
        if (child >= 0) stack.push_back({static_cast<u32>(child), world, props});
      }
      continue;
    }

    const GbShape* shape = scene.shapes.find(entry.block);
    if (!shape || shape->hidden) continue;
    if (shape->skin >= 0) {
      ++result.skipped_shapes;
      continue;
    }
    // Editor-only markers carry visible planes the game never draws.
    if (shape->name.find("EditorMarker") != std::string::npos) continue;
    const GbGeometry* geometry = scene.geometry.find(static_cast<u32>(shape->data));
    if (!geometry || geometry->vertices.empty() || geometry->indices.empty()) {
      ++result.skipped_shapes;
      continue;
    }
    PropertySet props = entry.props;
    props.Apply(scene, shape->properties);

    // Oblivion stores tangent space as binary extra data off the shape.
    const f32* extra_tangents = nullptr;
    for (i32 extra : shape->extra) {
      if (extra < 0) continue;
      if (const GbTangents* t = scene.tangents.find(static_cast<u32>(extra))) {
        if (t->size >= geometry->vertices.size() * 24) {
          extra_tangents = reinterpret_cast<const f32*>(t->data);
        }
      }
    }

    Transform world = Compose(entry.world, shape->local);
    u32 vertex_base = static_cast<u32>(lod.vertices.size());
    u32 index_offset = static_cast<u32>(lod.indices.size());
    for (size_t vi = 0; vi < geometry->vertices.size(); ++vi) {
      const asset::Vertex& src = geometry->vertices[vi];
      asset::Vertex v = src;
      v.color |= 0xff000000u;  // vertex alpha never feeds the alpha test
      if (extra_tangents) {
        std::memcpy(v.tangent, extra_tangents + vi * 3, 12);
        v.tangent[3] = 1;
      }
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
    submesh.material = material_for(props);
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

bool IsGamebryoNifVersion(ByteSpan data) {
  std::string_view text(reinterpret_cast<const char*>(data.data()),
                        std::min<size_t>(data.size(), 80));
  if (!text.starts_with("Gamebryo File Format") && !text.starts_with("NetImmerse File Format")) {
    return false;
  }
  size_t newline = text.find('\n');
  if (newline == std::string_view::npos || newline + 5 > data.size()) return false;
  u32 version;
  std::memcpy(&version, data.data() + newline + 1, 4);
  if (version == kV4_0_0_2) return true;   // Morrowind (NetImmerse)
  if (version == kV10_0_1_2) return true;  // Oblivion groundcover plants
  return version >= kV10_1_0_106 && version < 0x14010000;  // 10.1.0.106 .. 20.0.0.x
}

}  // namespace rx::bethesda
