#include "bethesda/nif.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <base/containers/unordered_map.h>
#include <base/option.h>

#include "asset/asset_id.h"
#include "core/log.h"

namespace rx::bethesda {
namespace {

// Refraction-flagged shapes (SLSF1 bits 15/16) route to the screen-space
// transmission path; off restores the old skip-them behaviour.
base::Option<bool> NifRefraction{"nif.refraction", true, "RX_NIF_REFRACTION",
                                 "render refraction-flagged nif shapes as transmission"};
// NiParticleSystem emitters parsed into asset::ParticleEmitter for the
// renderer's cpu billboard simulation (fires, smoke, mist).
base::Option<bool> NifParticles{"nif.particles", true, "RX_PARTICLES",
                                "parse nif particle systems into runtime emitters"};
// BSEffectShaderProperty geometry (torch/campfire flames, glow planes, mist
// sheets) rendered as unlit emissive transparents; off restores the skip.
base::Option<bool> NifEffectShaders{"nif.effect_shaders", true, "RX_EFFECT_SHADERS",
                                    "render nif effect-shader geometry as unlit emissive"};

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

// Rotation part of a NIF Transform to a quaternion, consistent with the
// engine MakeFromQuat (r[i*3+j] is row i, col j). Shoemake's branchy form.
Quat QuatFromTransform(const Transform& t) {
  const f32* m = t.r;  // m[row*3+col]
  f32 trace = m[0] + m[4] + m[8];
  Quat q;
  if (trace > 0) {
    f32 s = 0.5f / std::sqrt(trace + 1.0f);
    q.w = 0.25f / s;
    q.x = (m[7] - m[5]) * s;
    q.y = (m[2] - m[6]) * s;
    q.z = (m[3] - m[1]) * s;
  } else if (m[0] > m[4] && m[0] > m[8]) {
    f32 s = 2.0f * std::sqrt(1.0f + m[0] - m[4] - m[8]);
    q.w = (m[7] - m[5]) / s;
    q.x = 0.25f * s;
    q.y = (m[1] + m[3]) / s;
    q.z = (m[2] + m[6]) / s;
  } else if (m[4] > m[8]) {
    f32 s = 2.0f * std::sqrt(1.0f + m[4] - m[0] - m[8]);
    q.w = (m[2] - m[6]) / s;
    q.x = (m[1] + m[3]) / s;
    q.y = 0.25f * s;
    q.z = (m[5] + m[7]) / s;
  } else {
    f32 s = 2.0f * std::sqrt(1.0f + m[8] - m[0] - m[4]);
    q.w = (m[3] - m[1]) / s;
    q.x = (m[2] + m[6]) / s;
    q.y = (m[5] + m[7]) / s;
    q.z = 0.25f * s;
  }
  return Normalize(q);
}

// NIF Transform (row-major 3x3 rotation, uniform scale, translation) to the
// engine's column-major Mat4. Multiplying the Mat4 by (x,y,z,1) reproduces
// Transform::Apply, and Mat4 operator* matches Compose.
Mat4 ToMat4(const Transform& t) {
  Mat4 m = Mat4::Identity();
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) m.m[col * 4 + row] = t.r[row * 3 + col] * t.s;
  }
  m.m[12] = t.t[0];
  m.m[13] = t.t[1];
  m.m[14] = t.t[2];
  return m;
}

// NiAVObject prefix shared by nodes and shapes: name, extra data list,
// controller, flags, transform, collision object. Flag bit 0 is "hidden",
// which collision proxy meshes rely on.
Transform ReadAvObject(Reader& r, bool* hidden, i32* name_index = nullptr) {
  i32 name = r.Read<i32>();  // index into the header string table
  if (name_index) *name_index = name;
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
  std::string name;  // resolved from the header string table, for bone matching
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
  i32 skin = -1;        // NiSkinInstance/BSDismemberSkinInstance
  Geometry geometry;    // BSTriShape only
  bool hidden = false;
  bool skipped = false;
};

struct ShaderInfo {
  i32 texture_set = -1;
  u32 shader_type = 0;  // BSLightingShaderProperty type enum
  u32 flags1 = 0;
  u32 flags2 = 0;
  f32 emissive[3] = {0, 0, 0};
  f32 emissive_multiple = 1;
  f32 glossiness = 80;
  f32 refraction_strength = 0;  // screen distortion amount, SLSF1 refraction only
  f32 env_map_scale = -1;  // environment map reflectivity (type 1); -1 = absent
  bool effect = false;
  bool water = false;
  std::string effect_texture;
  std::string greyscale_texture;  // effect grayscale-to-palette lookup
  // BSEffectShaderProperty (Skyrim layout): emissive colour (rgba) * multiple,
  // view-angle falloff, soft-particle depth. Only used when `effect`.
  f32 effect_color[4] = {1, 1, 1, 1};
  f32 effect_scale = 1;
  f32 falloff[4] = {1, 1, 1, 1};  // start angle, stop angle, start op, stop op
  f32 soft_falloff_depth = 0;
  std::string material_file;  // FO4 .bgsm/.bgem referenced by the shader name
  i32 controller = -1;        // NiTimeController chain head off the property
  f32 uv_scroll_u = 0;        // resolved animated scroll rate (uv units/sec)
  f32 uv_scroll_v = 0;
  f32 emissive_pulse[2] = {0, 0};  // x frequency (Hz), y amount; from a controller
};

// BS{Lighting,Effect}ShaderPropertyFloatController: a NiFloatInterpController
// whose interpolator drives one shader float (the controlled variable). The
// controlled-variable enums differ between the two shader types (nif.xml
// LightingShaderControlledFloat vs EffectShaderControlledVariable).
struct FloatController {
  i32 next = -1;         // NiTimeController::Next Controller
  i32 interpolator = -1;
  u32 variable = 0;      // controlled variable enum
  bool effect = false;   // enum is EffectShaderControlledVariable when set
};

// NiFloatData reduced to endpoints: rate = (last.value - first.value) /
// (last.time - first.time) gives a constant scroll speed for a cyclic
// monotonic-offset controller.
struct FloatKeys {
  u32 count = 0;
  f32 first_time = 0, first_value = 0;
  f32 last_time = 0, last_value = 0;
  f32 min_value = 0, max_value = 0;  // over all keys, for cyclic pulse amplitude
};

// A NIF shader name may name a Fallout 4 material file ("materials\...\x.bgsm").
// Folds it to the "materials/..." vfs key; returns empty for non-material names.
std::string MaterialFilePath(std::string_view raw) {
  if (raw.empty()) return {};
  std::string path = asset::NormalizePath(raw);
  if (!path.ends_with(".bgsm") && !path.ends_with(".bgem")) return {};
  size_t anchor = path.rfind("materials/");
  return anchor != std::string::npos ? path.substr(anchor) : "materials/" + path;
}

// Refraction shapes (heat haze, glass distortion) carry a normal map in the
// diffuse slot for the distortion effect; they render via the screen-space
// transmission path, never binding that map as a diffuse.
constexpr u32 kShaderFlag1Refraction = 1u << 15;
constexpr u32 kShaderFlag1FireRefraction = 1u << 16;

// BSLightingShaderProperty flag bits (nif.xml SkyrimShaderPropertyFlags1/2).
constexpr u32 kShaderFlag1GrayscaleToColor = 1u << 4;
constexpr u32 kShaderFlag1GrayscaleToAlpha = 1u << 5;
constexpr u32 kShaderFlag1UseFalloff = 1u << 6;
constexpr u32 kShaderFlag1EnvMapping = 1u << 7;
constexpr u32 kShaderFlag1EyeEnvMapping = 1u << 17;
constexpr u32 kShaderFlag1OwnEmit = 1u << 22;
constexpr u32 kShaderFlag1SoftEffect = 1u << 30;
constexpr u32 kShaderFlag2DoubleSided = 1u << 4;
constexpr u32 kShaderFlag2GlowMap = 1u << 6;
constexpr u32 kShaderFlag2TreeAnim = 1u << 29;

// Skyrim shader types whose texture set holds diffuse/normal in slots 0/1.
// The others (landscape multitexture, LOD, world map) repurpose the slots
// and would bind garbage as a diffuse.
bool ShaderTypeUsesDiffuseSlot(u32 type) {
  switch (type) {
    case 0:   // default
    case 1:   // environment map
    case 2:   // glow
    case 3:   // parallax
    case 4:   // face tint
    case 5:   // skin tint
    case 6:   // hair tint
    case 7:   // parallax occlusion
    case 10:  // snow
    case 11:  // multilayer parallax
    case 12:  // tree anim
    case 14:  // sparkle snow
    case 16:  // eye envmap
    case 18:  // lod landscape (.btr): the baked diffuse atlas + normal
      return true;
    default:
      return false;
  }
}

struct AlphaInfo {
  u16 flags = 0;
  u8 threshold = 128;
};

// NiParticleSystem / BSStripParticleSystem (BS stream >= 100 layout, verified
// against nifskope's nif.xml): NiAVObject prefix, NiBound, skin ref, shader +
// alpha refs, vertex desc, far/near fade shorts, NiPSysData ref, world-space
// flag, then the modifier list carrying the emitters and forces.
struct PsysBlock {
  Transform local;
  i32 shader = -1;
  i32 alpha = -1;
  i32 data = -1;  // NiPSysData
  base::Vector<i32> modifiers;
  bool hidden = false;
};

// NiPSysBox/Cylinder/SphereEmitter, reduced to the shared NiPSysEmitter
// fields plus the volume folded into axis-aligned half extents.
struct EmitterBlock {
  f32 speed = 0, speed_variation = 0;
  f32 declination_variation = 0;
  f32 color[4] = {1, 1, 1, 1};
  f32 radius = 1;
  f32 life = 1, life_variation = 0;
  i32 emitter_object = -1;  // NiNode the volume hangs off, -1 = the system
  f32 half_extent[3] = {0, 0, 0};
  bool is_mesh = false;          // NiPSysMeshEmitter: emit from a mesh's volume
  base::Vector<i32> mesh_refs;   // shapes emitted from (mesh emitter)
};

// BSPSysSimpleColorModifier: three colours interpolated across the particle's
// life with an alpha fade in/out. keys are life fractions.
struct ColorModBlock {
  f32 key[6] = {0, 1, 0.33f, 0.33f, 0.66f, 0.66f};
  f32 color[3][4] = {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}};
};

struct GravityBlock {
  i32 object = -1;  // orientation source for the axis
  f32 axis[3] = {0, 0, 1};
  f32 strength = 0;
};

// NiPSysEmitterCtlr: target particle system + the birth-rate interpolator
// (NiFloatInterpolator -> NiFloatData, resolved through the existing maps).
struct EmitterCtlrBlock {
  i32 target = -1;
  i32 interpolator = -1;
};

// NiFloatInterpolator: a keyed NiFloatData ref, or just the pose value when
// the driven quantity is constant (most emitter birth rates).
struct FloatInterp {
  f32 pose = 0;
  i32 data = -1;
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

// Per vertex skinning attributes pulled out of a packed vertex buffer.
struct SkinVertexData {
  base::Vector<u8> bone_indices;  // 4 per vertex
  base::Vector<f32> weights;      // 4 per vertex
};

void DecodePackedVertices(const VertexLayout& layout, const u8* base, u32 vertex_count,
                          base::Vector<asset::Vertex>* vertices, SkinVertexData* skin) {
  vertices->resize(vertex_count);
  if (skin) {
    skin->bone_indices.resize(vertex_count * 4);
    skin->weights.resize(vertex_count * 4);
  }
  for (u32 i = 0; i < vertex_count; ++i) {
    const u8* v = base + i * layout.stride;
    asset::Vertex& vertex = (*vertices)[i];
    // Dynamic shapes (head, hair) keep positions in a separate array, so their
    // partition buffer carries UV/normal/tangent at offset 0 with no position
    // flag; only read a position when one is actually present.
    if (layout.flags & VertexLayout::kHasVertex) {
      if (layout.full_precision) {
        std::memcpy(vertex.position, v, 12);
      } else {
        u16 h[3];
        std::memcpy(h, v, 6);
        for (int k = 0; k < 3; ++k) vertex.position[k] = HalfToFloat(h[k]);
      }
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
      // Real rgba is kept here; the flatten loop forces alpha opaque for
      // everything except effect-shader planes (which fade through it).
      std::memcpy(&vertex.color, v + layout.color, 4);
    }
    if (skin && (layout.flags & VertexLayout::kSkinned) && layout.skin != 0) {
      u16 w[4];
      std::memcpy(w, v + layout.skin, 8);
      for (int k = 0; k < 4; ++k) {
        skin->weights[i * 4 + k] = HalfToFloat(w[k]);
        skin->bone_indices[i * 4 + k] = v[layout.skin + 8 + k];
      }
    }
  }
}

// FO4 (BS stream >= 130) widened the triangle count to u32; SSE (100) keeps it
// u16. Everything else in the geometry header is shared.
bool ReadBsTriShapeGeometry(Reader& r, u32 bs_version, Geometry* out) {
  u64 desc = r.Read<u64>();
  u32 triangle_count = bs_version >= 130 ? r.Read<u32>() : r.Read<u16>();
  u32 vertex_count = r.Read<u16>();
  u32 data_size = r.Read<u32>();
  if (!r.ok || data_size == 0 || vertex_count == 0) return false;

  VertexLayout layout(desc);
  if (layout.flags & VertexLayout::kSkinned) return false;
  if (!(layout.flags & VertexLayout::kHasVertex) || layout.stride == 0) return false;
  if (data_size != layout.stride * vertex_count + 6 * triangle_count) return false;

  const u8* base = r.Bytes(data_size);
  if (!base) return false;

  DecodePackedVertices(layout, base, vertex_count, &out->vertices, nullptr);

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

// BSDynamicTriShape (head, hair): the BSTriShape data carries normals/uv/
// triangles (positions may be zero), then a dynamic Vector4 array holds the
// real positions. Skinned layouts are fine; the caller rigid-attaches the mesh.
bool ReadBsDynamicTriShape(Reader& r, u32 bs_version, Geometry* out) {
  u64 desc = r.Read<u64>();
  u32 triangle_count = bs_version >= 130 ? r.Read<u32>() : r.Read<u16>();
  u32 vertex_count = r.Read<u16>();
  u32 data_size = r.Read<u32>();
  if (!r.ok || vertex_count == 0) return false;

  VertexLayout layout(desc);
  if ((layout.flags & VertexLayout::kHasVertex) && layout.stride != 0 && data_size != 0 &&
      data_size == layout.stride * vertex_count + 6 * triangle_count) {
    const u8* base = r.Bytes(data_size);
    if (!base) return false;
    DecodePackedVertices(layout, base, vertex_count, &out->vertices, nullptr);
    const u8* tris = base + layout.stride * vertex_count;
    out->indices.resize(triangle_count * 3);
    for (u32 i = 0; i < triangle_count * 3; ++i) {
      u16 index;
      std::memcpy(&index, tris + i * 2, 2);
      if (index >= vertex_count) return false;
      out->indices[i] = index;
    }
  } else {
    if (data_size) r.Skip(data_size);
    out->vertices.resize(vertex_count);
  }

  // Dynamic vertices: an 8 byte header (a reserved u32 plus the vertex-data-size
  // u32, = 16 * vertex_count) precedes the Vector4 per vertex array (w unused).
  // These are the live positions; triangles come from the skin partition.
  r.Skip(8);
  const u8* dyn = r.Bytes(16 * vertex_count);
  if (!dyn) return false;
  for (u32 i = 0; i < vertex_count; ++i) {
    std::memcpy(out->vertices[i].position, dyn + i * 16, 12);
  }
  return !out->vertices.empty();
}

// NiSkinInstance / BSDismemberSkinInstance shared prefix.
struct SkinInstanceBlock {
  i32 data = -1;       // NiSkinData
  i32 partition = -1;  // NiSkinPartition
  base::Vector<i32> bones;
};

bool ReadSkinInstance(Reader& r, SkinInstanceBlock* out) {
  out->data = r.Read<i32>();
  out->partition = r.Read<i32>();
  r.Skip(4);  // skeleton root
  u32 bone_count = r.Read<u32>();
  if (!r.ok || bone_count > 4096) return false;
  out->bones.reserve(bone_count);
  for (u32 i = 0; i < bone_count; ++i) out->bones.push_back(r.Read<i32>());
  return r.ok;
}

// Rotation-first NiTransform as used by NiSkinData, unlike the AV object
// order (translation first).
Transform ReadSkinTransform(Reader& r) {
  Transform t;
  for (f32& v : t.r) v = r.Read<f32>();
  for (f32& v : t.t) v = r.Read<f32>();
  t.s = r.Read<f32>();
  return t;
}

// NiSkinData: per bone skin-to-bone bind transforms. The per bone vertex
// weight lists are skipped; SSE keeps the authoritative weights in the
// partition vertex data.
bool ReadSkinData(Reader& r, base::Vector<Transform>* skin_to_bone) {
  r.Skip(52);  // overall skin transform, folded into the per bone transforms
  u32 bone_count = r.Read<u32>();
  bool has_weights = r.Read<u8>() != 0;
  if (!r.ok || bone_count > 4096) return false;
  skin_to_bone->reserve(bone_count);
  for (u32 i = 0; i < bone_count; ++i) {
    skin_to_bone->push_back(ReadSkinTransform(r));
    r.Skip(16);  // bounding sphere
    u32 vertex_count = r.Read<u16>();
    if (has_weights) r.Skip(6 * vertex_count);
    if (!r.ok) return false;
  }
  return true;
}

// SSE NiSkinPartition (BS stream 100): one shared packed vertex buffer plus
// partitions with their own bone palettes and triangles in global vertex
// indices.
struct SkinPartitionBlock {
  base::Vector<asset::Vertex> vertices;
  SkinVertexData skin;
  base::Vector<u32> indices;
  struct Span {
    base::Vector<u16> bones;
    u32 first_index = 0;
    u32 index_count = 0;
  };
  base::Vector<Span> spans;
};

bool ReadSkinPartition(Reader& r, u32 bs_version, SkinPartitionBlock* out) {
  if (bs_version != 100) return false;
  u32 partition_count = r.Read<u32>();
  u32 data_size = r.Read<u32>();
  u32 vertex_size = r.Read<u32>();
  u64 desc = r.Read<u64>();
  if (!r.ok || partition_count > 4096) return false;
  // A dynamic shape (head, hair) keeps its vertices in the shape block, so the
  // partition's shared buffer is empty; only the triangles matter then.
  // A dynamic shape (head, hair) keeps its positions in the shape block, so the
  // partition buffer has no vertex flag; we still want its triangles.
  bool has_buffer = vertex_size != 0 && data_size != 0;
  u32 vertex_count = has_buffer ? data_size / vertex_size : 0;
  if (has_buffer) {
    VertexLayout layout(desc);
    if (data_size % vertex_size != 0 || layout.stride != vertex_size) return false;
    const u8* base = r.Bytes(data_size);
    if (!base || vertex_count == 0) return false;
    // Decode even without a position flag: dynamic shapes still carry their
    // UV/normal/tangent/color here (the shape block supplies the positions).
    DecodePackedVertices(layout, base, vertex_count, &out->vertices, &out->skin);
  }

  for (u32 p = 0; p < partition_count; ++p) {
    u32 part_vertices = r.Read<u16>();
    u32 part_triangles = r.Read<u16>();
    u32 part_bones = r.Read<u16>();
    u32 part_strips = r.Read<u16>();
    u32 weights_per_vertex = r.Read<u16>();
    if (!r.ok) return false;
    SkinPartitionBlock::Span span;
    span.bones.reserve(part_bones);
    for (u32 b = 0; b < part_bones; ++b) span.bones.push_back(r.Read<u16>());
    if (r.Read<u8>() != 0) r.Skip(2 * part_vertices);                       // vertex map
    if (r.Read<u8>() != 0) r.Skip(4 * part_vertices * weights_per_vertex);  // weights
    u32 strip_total = 0;
    for (u32 s = 0; s < part_strips; ++s) strip_total += r.Read<u16>();
    if (r.Read<u8>() != 0) {  // has faces
      r.Skip(part_strips == 0 ? 6 * part_triangles : 2 * strip_total);
    }
    if (r.Read<u8>() != 0) r.Skip(part_vertices * weights_per_vertex);  // bone indices
    r.Skip(2 + 8);  // unknown short, per partition vertex desc
    if (!r.ok) return false;
    span.first_index = static_cast<u32>(out->indices.size());
    span.index_count = part_triangles * 3;
    const u8* tris = r.Bytes(6 * part_triangles);
    if (!tris) return false;
    for (u32 i = 0; i < part_triangles * 3; ++i) {
      u16 index;
      std::memcpy(&index, tris + i * 2, 2);
      if (has_buffer && index >= vertex_count) return false;
      out->indices.push_back(index);
    }
    out->spans.push_back(std::move(span));
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
      // Real alpha kept; the flatten loop forces it opaque except on effects.
      out->vertices[i].color = pack(c[0]) | pack(c[1]) << 8 | pack(c[2]) << 16 | pack(c[3]) << 24;
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
  header.strings.reserve(string_count);
  for (u32 i = 0; i < string_count; ++i) header.strings.push_back(ReadSizedString(r));
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

static NifConversion ConvertNifImpl(ByteSpan data, asset::AssetId id, std::string_view source_path,
                                    bool keep_skin, bool rigid_fallback) {
  NifConversion result;
  auto header = ParseNifHeader(data);
  if (!header) {
    RX_WARN("unsupported nif: {}", source_path);
    return result;
  }

  u32 block_count = static_cast<u32>(header->block_sizes.size());
  base::UnorderedMap<u32, Node> nodes;
  base::UnorderedMap<u32, Shape> shapes;
  base::UnorderedMap<u32, Geometry> geometry_blocks;
  base::UnorderedMap<u32, ShaderInfo> shaders;
  base::UnorderedMap<u32, AlphaInfo> alphas;
  base::UnorderedMap<u32, base::Vector<std::string>> texture_sets;
  base::UnorderedMap<u32, SkinInstanceBlock> skin_instances;
  base::UnorderedMap<u32, base::Vector<Transform>> skin_datas;
  base::UnorderedMap<u32, SkinPartitionBlock> skin_partitions;
  base::UnorderedMap<u32, FloatController> controllers;
  base::UnorderedMap<u32, FloatInterp> interp_data;  // NiFloatInterpolator blocks
  base::UnorderedMap<u32, FloatKeys> float_datas;
  base::UnorderedMap<u32, PsysBlock> psys_blocks;
  base::UnorderedMap<u32, EmitterBlock> emitter_blocks;
  base::UnorderedMap<u32, ColorModBlock> color_mods;
  base::UnorderedMap<u32, GravityBlock> gravity_blocks;
  // NiPSysData: pool size plus the subtexture (flipbook) grid when authored.
  struct PsysData {
    u16 max_particles = 0;
    u16 frames = 1;  // subtexture offsets in the atlas, 1 = whole texture
    u8 cols = 1, rows = 1;
  };
  base::UnorderedMap<u32, PsysData> psys_max;
  base::Vector<EmitterCtlrBlock> emitter_ctlrs;

  for (u32 i = 0; i < block_count; ++i) {
    const std::string& type = header->block_types[header->block_type_index[i]];
    Reader r{data.subspan(header->block_offsets[i], header->block_sizes[i])};

    if (type.ends_with("Node")) {
      Node node;
      i32 name_index = -1;
      node.local = ReadAvObject(r, &node.hidden, &name_index);
      if (name_index >= 0 && static_cast<u32>(name_index) < header->strings.size()) {
        node.name = header->strings[name_index];
      }
      u32 child_count = r.Read<u32>();
      if (!r.ok || child_count > 65536) continue;
      node.children.reserve(child_count);
      for (u32 c = 0; c < child_count; ++c) node.children.push_back(r.Read<i32>());
      if (type == "NiSwitchNode" && !node.children.empty()) {
        // Only the active child renders (trees keep an animated and a static
        // variant side by side).
        u32 effect_count = r.Read<u32>();
        if (effect_count > 4096) effect_count = 0;
        r.Skip(4 * effect_count + 2);  // effect refs, switch flags
        u32 index = r.Read<u32>();
        if (!r.ok || index >= node.children.size()) index = 0;
        i32 active = node.children[index];
        node.children.clear();
        node.children.push_back(active);
        r.ok = true;
      } else if (type == "NiLODNode" && !node.children.empty()) {
        // Distance-switched detail levels (children[i] pairs with lod range i,
        // child 0 starting at the camera). Without this every level would render
        // stacked. It's 2026 — always keep the finest (child 0).
        i32 finest = node.children[0];
        node.children.clear();
        node.children.push_back(finest);
      }
      if (r.ok) nodes.emplace(i, std::move(node));
    } else if (type == "BSTriShape" || type == "BSMeshLODTriShape" ||
               type == "BSSubIndexTriShape") {
      // BSSubIndexTriShape (FO4 static meshes) shares the BSTriShape geometry
      // header; its trailing segment table sits after the triangles, which the
      // geometry read stops short of, so we treat it identically.
      // BSMeshLODTriShape likewise: its trailing lod0/lod1/lod2 index counts are
      // shorter prefixes of the full triangle list, so reading the whole list
      // keeps the finest detail.
      Shape shape;
      shape.local = ReadAvObject(r, &shape.hidden);
      r.Skip(16);  // bounding sphere
      shape.skin = r.Read<i32>();
      shape.shader = r.Read<i32>();
      shape.alpha = r.Read<i32>();
      if (!r.ok) continue;
      if (!ReadBsTriShapeGeometry(r, header->bs_version, &shape.geometry) && shape.skin < 0)
        shape.skipped = true;
      shapes.emplace(i, std::move(shape));
    } else if (type == "BSDynamicTriShape") {
      // Head and hair: positions live in a dynamic array, so the geometry is
      // read inline (not skinned); the actor loader rigid-attaches it.
      Shape shape;
      shape.local = ReadAvObject(r, &shape.hidden);
      r.Skip(16);  // bounding sphere
      shape.skin = r.Read<i32>();
      shape.shader = r.Read<i32>();
      shape.alpha = r.Read<i32>();
      if (!r.ok) continue;
      if (!ReadBsDynamicTriShape(r, header->bs_version, &shape.geometry)) shape.skipped = true;
      shapes.emplace(i, std::move(shape));
    } else if (type == "NiTriShape") {
      Shape shape;
      shape.local = ReadAvObject(r, &shape.hidden);
      shape.data = r.Read<i32>();
      shape.skin = r.Read<i32>();  // kept so the rigid path can fall back to it
      u32 material_count = r.Read<u32>();
      if (!r.ok || material_count > 4096) continue;
      r.Skip(8 * material_count + 4 + 1);  // names+extra, active material, needs update
      shape.shader = r.Read<i32>();
      shape.alpha = r.Read<i32>();
      if (!r.ok) continue;
      shapes.emplace(i, std::move(shape));
    } else if (type == "NiTriShapeData") {
      Geometry geometry;
      if (ReadNiTriShapeData(r, header->bs_version, &geometry)) {
        geometry_blocks.emplace(i, std::move(geometry));
      }
    } else if (type == "NiTriStrips") {
      ++result.skipped_shapes;
    } else if (type == "NiSkinInstance" || type == "BSDismemberSkinInstance") {
      SkinInstanceBlock skin;
      if (ReadSkinInstance(r, &skin)) skin_instances.emplace(i, std::move(skin));
    } else if (type == "NiSkinData") {
      base::Vector<Transform> skin_to_bone;
      if (ReadSkinData(r, &skin_to_bone)) skin_datas.emplace(i, std::move(skin_to_bone));
    } else if (type == "NiSkinPartition") {
      SkinPartitionBlock partition;
      if (ReadSkinPartition(r, header->bs_version, &partition)) {
        skin_partitions.emplace(i, std::move(partition));
      }
    } else if (type == "BSLightingShaderProperty") {
      ShaderInfo info;
      info.shader_type = r.Read<u32>();
      i32 name_index = r.Read<i32>();  // FO4: names the .bgsm material file
      if (name_index >= 0 && static_cast<u32>(name_index) < header->strings.size()) {
        info.material_file = MaterialFilePath(header->strings[name_index]);
      }
      u32 extra = r.Read<u32>();
      if (!r.ok || extra > 4096) continue;
      r.Skip(4 * extra);  // extra refs
      info.controller = r.Read<i32>();
      info.flags1 = r.Read<u32>();
      info.flags2 = r.Read<u32>();
      r.Skip(16);  // uv offset + scale
      info.texture_set = r.Read<i32>();
      for (f32& v : info.emissive) v = r.Read<f32>();
      info.emissive_multiple = r.Read<f32>();
      r.Skip(4 + 4);  // clamp mode, alpha
      info.refraction_strength = r.Read<f32>();
      info.glossiness = r.Read<f32>();
      bool base_ok = r.ok;
      // Environment Map Scale follows the specular + lighting-effect block for
      // the environment-map shader type (Skyrim layout; FO4 (bs >= 130) binds
      // reflectivity through its .bgsm material instead). A parse overrun here
      // must not drop the whole shader, so gate the emplace on base_ok.
      if (base_ok && info.shader_type == 1 && header->bs_version < 130) {
        r.Skip(12 + 4 + 4 + 4);  // specular color, strength, lighting effect 1/2
        f32 scale = r.Read<f32>();
        if (r.ok) info.env_map_scale = scale;
      }
      if (base_ok) shaders.emplace(i, info);
    } else if (type == "BSWaterShaderProperty") {
      // Placed water (river rapids, waterfalls pools): routed to the
      // engine's water pipeline through a synthesized water material.
      ShaderInfo info;
      info.water = true;
      shaders.emplace(i, info);
    } else if (type == "BSEffectShaderProperty") {
      ShaderInfo info;
      info.effect = true;
      i32 name_index = r.Read<i32>();  // FO4: names the .bgem material file
      if (name_index >= 0 && static_cast<u32>(name_index) < header->strings.size()) {
        info.material_file = MaterialFilePath(header->strings[name_index]);
      }
      u32 extra = r.Read<u32>();
      if (!r.ok || extra > 4096) continue;
      r.Skip(4 * extra);  // extra refs
      info.controller = r.Read<i32>();
      info.flags1 = r.Read<u32>();
      info.flags2 = r.Read<u32>();
      r.Skip(16);  // uv offset + scale
      info.effect_texture = ReadSizedString(r);
      // Full Skyrim BSEffectShaderProperty tail (nifly Shaders.cpp, stream 100):
      // texture clamp mode, falloff angles/opacities, base (emissive) colour +
      // scale, soft-falloff depth, then the greyscale palette. FO4/FO76
      // (bs >= 130) reorder these and add texture slots, so keep the source
      // texture only there (they bind their .bgem anyway).
      if (header->bs_version < 130) {
        r.Skip(4);  // texture clamp mode
        for (f32& v : info.falloff) v = r.Read<f32>();
        for (f32& c : info.effect_color) c = r.Read<f32>();  // base colour rgba
        info.effect_scale = r.Read<f32>();                   // base colour scale
        info.soft_falloff_depth = r.Read<f32>();
        info.greyscale_texture = ReadSizedString(r);
      }
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
    } else if (type == "BSLightingShaderPropertyFloatController" ||
               type == "BSEffectShaderPropertyFloatController") {
      // NiTimeController base: next, flags(u16), freq, phase, start, stop,
      // target ptr; then NiSingleInterpController's interpolator ref and the
      // controlled-variable enum.
      FloatController c;
      c.effect = type.starts_with("BSEffect");
      c.next = r.Read<i32>();
      r.Skip(2 + 16 + 4);  // flags, freq/phase/start/stop, target
      c.interpolator = r.Read<i32>();
      c.variable = r.Read<u32>();
      if (r.ok) controllers.emplace(i, c);
    } else if (type == "NiFloatInterpolator") {
      FloatInterp interp;
      interp.pose = r.Read<f32>();  // constant value when there is no key data
      interp.data = r.Read<i32>();
      if (r.ok) interp_data.emplace(i, interp);
    } else if (type == "NiFloatData") {
      // KeyGroup<float>: num keys, interpolation type (if any), then keys.
      u32 num_keys = r.Read<u32>();
      if (r.ok && num_keys > 0 && num_keys < 1000000) {
        u32 interpolation = r.Read<u32>();
        u32 stride = interpolation == 2 ? 16 : (interpolation == 3 ? 20 : 8);
        FloatKeys keys;
        keys.count = num_keys;
        // Walk every key: endpoints give the scroll rate, min/max the pulse
        // amplitude for a cyclic emissive-multiple controller.
        for (u32 k = 0; k < num_keys && r.ok; ++k) {
          f32 t = r.Read<f32>();
          f32 v = r.Read<f32>();
          r.Skip(stride - 8);  // tangents / tbc, unused
          if (k == 0) {
            keys.first_time = t;
            keys.first_value = keys.min_value = keys.max_value = v;
          }
          keys.last_time = t;
          keys.last_value = v;
          keys.min_value = std::min(keys.min_value, v);
          keys.max_value = std::max(keys.max_value, v);
        }
        if (r.ok) float_datas.emplace(i, keys);
      }
    } else if (type == "NiParticleSystem" || type == "BSStripParticleSystem") {
      // Pre-SSE streams lay the system out through NiGeometry's data ref
      // instead; only the BS >= 100 form is parsed.
      if (header->bs_version < 100) continue;
      PsysBlock p;
      p.local = ReadAvObject(r, &p.hidden);
      r.Skip(16);  // bounding sphere
      if (header->bs_version == 155) r.Skip(24);  // FO76 bound min/max
      r.Skip(4);   // skin ref
      p.shader = r.Read<i32>();
      p.alpha = r.Read<i32>();
      r.Skip(8 + 8);  // vertex desc, far/near fade shorts
      p.data = r.Read<i32>();
      r.Skip(1);  // world space flag
      u32 modifier_count = r.Read<u32>();
      if (!r.ok || modifier_count > 256) continue;
      p.modifiers.reserve(modifier_count);
      for (u32 m = 0; m < modifier_count; ++m) p.modifiers.push_back(r.Read<i32>());
      if (r.ok) psys_blocks.emplace(i, std::move(p));
    } else if (type == "NiPSysData") {
      r.Skip(4);  // group id
      PsysData pd;
      pd.max_particles = r.Read<u16>();  // "BS Max Vertices"
      bool base_ok = r.ok;
      // SSE (20.2.0.7, stream 100) NiPSysData tail through NiGeometryData /
      // NiParticlesData (nifly Geometry.cpp/Particles.cpp): the per-vertex
      // arrays are all elided for particle systems, leaving fixed-size fields
      // up to the subtexture-offset (flipbook) table.
      if (header->bs_version == 100) {
        // keep/compress/hasVertices, dataFlags, materialCRC, hasNormals,
        // bounds, hasVertexColors, consistencyFlags, additionalDataRef.
        r.Skip(3 + 2 + 4 + 1 + 16 + 1 + 2 + 4);
        // hasRadii, numActive, hasSizes, hasRotations, hasRotationAngles,
        // hasRotationAxes, hasTextureIndices.
        r.Skip(1 + 2 + 1 + 1 + 1 + 1 + 1);
        u32 subtex_count = r.Read<u32>();
        if (r.ok && subtex_count > 1 && subtex_count <= 64) {
          // Uniform-grid atlas by convention: each offset is (u, v, w, h)
          // normalized; the first rect's size gives the grid dimensions.
          // Verified layout: each offset is (u_min, u_max, v_min, v_max); the
          // first cell's extents give the uniform grid (fire 16 frames ships
          // (0, 0.25, 0, 0.25) = 4x4).
          f32 rect[4];
          for (f32& v : rect) v = r.Read<f32>();
          f32 cell_w = rect[1] - rect[0];
          f32 cell_h = rect[3] - rect[2];
          if (r.ok && cell_w > 1e-3f && cell_h > 1e-3f) {
            u32 cols = static_cast<u32>(std::lround(1.0f / cell_w));
            u32 rows = static_cast<u32>(std::lround(1.0f / cell_h));
            if (cols >= 1 && cols <= 15 && rows >= 1 && rows <= 15 &&
                cols * rows >= subtex_count) {
              pd.frames = static_cast<u16>(subtex_count);
              pd.cols = static_cast<u8>(cols);
              pd.rows = static_cast<u8>(rows);
            }
          }
        }
        r.ok = true;  // the flipbook probe is best-effort; keep max_particles
      }
      if (base_ok) psys_max.emplace(i, pd);
    } else if (type == "NiPSysBoxEmitter" || type == "NiPSysCylinderEmitter" ||
               type == "NiPSysSphereEmitter") {
      EmitterBlock e;
      r.Skip(4 + 4 + 4 + 1);  // modifier name, order, target, active
      e.speed = r.Read<f32>();
      e.speed_variation = r.Read<f32>();
      r.Skip(4);  // declination (emit axis stays +z)
      e.declination_variation = r.Read<f32>();
      r.Skip(8);  // planar angle + variation
      for (f32& c : e.color) c = r.Read<f32>();
      e.radius = r.Read<f32>();
      r.Skip(4);  // radius variation
      e.life = r.Read<f32>();
      e.life_variation = r.Read<f32>();
      e.emitter_object = r.Read<i32>();
      if (type == "NiPSysBoxEmitter") {
        for (f32& v : e.half_extent) v = r.Read<f32>() * 0.5f;
      } else if (type == "NiPSysCylinderEmitter") {
        f32 cyl_radius = r.Read<f32>();
        e.half_extent[0] = cyl_radius;
        e.half_extent[1] = cyl_radius;
        e.half_extent[2] = r.Read<f32>() * 0.5f;
      } else {
        f32 sphere_radius = r.Read<f32>();
        for (f32& v : e.half_extent) v = sphere_radius;
      }
      if (r.ok) emitter_blocks.emplace(i, e);
    } else if (type == "NiPSysMeshEmitter") {
      // Emits from the surface of one or more referenced meshes (big fires
      // burning along logs). Shares the NiPSysEmitter fields, then a mesh-ref
      // list and emission axis (unread). MVP: emit from the referenced mesh's
      // bounding volume.
      EmitterBlock e;
      e.is_mesh = true;
      r.Skip(4 + 4 + 4 + 1);  // modifier name, order, target, active
      e.speed = r.Read<f32>();
      e.speed_variation = r.Read<f32>();
      r.Skip(4);  // declination
      e.declination_variation = r.Read<f32>();
      r.Skip(8);  // planar angle + variation
      for (f32& c : e.color) c = r.Read<f32>();
      e.radius = r.Read<f32>();
      r.Skip(4);  // radius variation
      e.life = r.Read<f32>();
      e.life_variation = r.Read<f32>();
      u32 mesh_count = r.Read<u32>();
      if (!r.ok || mesh_count > 256) continue;
      for (u32 m = 0; m < mesh_count; ++m) e.mesh_refs.push_back(r.Read<i32>());
      if (r.ok) emitter_blocks.emplace(i, std::move(e));
    } else if (type == "BSPSysSimpleColorModifier") {
      ColorModBlock c;
      r.Skip(4 + 4 + 4 + 1);  // modifier name, order, target, active
      for (f32& k : c.key) k = r.Read<f32>();
      for (int j = 0; j < 3; ++j) {
        for (f32& v : c.color[j]) v = r.Read<f32>();
      }
      if (r.ok) color_mods.emplace(i, c);
    } else if (type == "NiPSysGravityModifier") {
      GravityBlock g;
      r.Skip(4 + 4 + 4 + 1);  // modifier name, order, target, active
      g.object = r.Read<i32>();
      for (f32& v : g.axis) v = r.Read<f32>();
      r.Skip(4);  // decay
      g.strength = r.Read<f32>();
      if (r.ok) gravity_blocks.emplace(i, g);
    } else if (type == "NiPSysEmitterCtlr") {
      // NiTimeController base, then the birth-rate interpolator ref (the
      // modifier name and visibility interpolator follow, unread).
      r.Skip(4 + 2 + 16);  // next, flags, freq/phase/start/stop
      i32 target = r.Read<i32>();
      i32 interpolator = r.Read<i32>();
      if (r.ok) emitter_ctlrs.push_back({target, interpolator});
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

  // Resolve each shader's float-controller chain into a constant uv scroll
  // rate. Walk NiTimeController::Next; for a U/V-Offset controller reduce its
  // NiFloatData keys to rate = (last - first) / duration (the cyclic monotonic
  // case: waterfalls, rivers, lava, conveyor runes).
  for (auto entry : shaders) {
    ShaderInfo& info = entry.value;
    i32 cur = info.controller;
    for (int guard = 0; cur >= 0 && guard < 64; ++guard) {
      const FloatController* c = controllers.find(static_cast<u32>(cur));
      if (!c) break;
      cur = c->next;
      const FloatInterp* interp =
          c->interpolator >= 0 ? interp_data.find(static_cast<u32>(c->interpolator)) : nullptr;
      const FloatKeys* keys =
          interp && interp->data >= 0 ? float_datas.find(static_cast<u32>(interp->data)) : nullptr;
      if (!keys || keys->count < 2) continue;
      f32 duration = keys->last_time - keys->first_time;
      if (duration <= 1e-4f) continue;
      f32 rate = (keys->last_value - keys->first_value) / duration;
      // Controlled-variable enums (nif.xml): lighting U/V Offset = 20/22,
      // effect U/V Offset = 6/8; emissive multiple = 11 (lighting) / 0 (effect).
      u32 u_offset = c->effect ? 6u : 20u;
      u32 v_offset = c->effect ? 8u : 22u;
      u32 emissive_mul = c->effect ? 0u : 11u;
      if (c->variable == u_offset && rate != 0.0f) {
        info.uv_scroll_u = rate;
        RX_INFO("nif: uv scroll u={}/s {}", rate, source_path);
      } else if (c->variable == v_offset && rate != 0.0f) {
        info.uv_scroll_v = rate;
        RX_INFO("nif: uv scroll v={}/s {}", rate, source_path);
      } else if (c->variable == emissive_mul && keys->max_value > keys->min_value) {
        // Cyclic glow pulse: modulate emission by a sine around the key mean.
        f32 mean = (keys->max_value + keys->min_value) * 0.5f;
        f32 amount = mean > 1e-4f ? (keys->max_value - keys->min_value) * 0.5f / mean : 0.0f;
        info.emissive_pulse[0] = 1.0f / duration;  // one cycle over the key span
        info.emissive_pulse[1] = std::clamp(amount, 0.0f, 1.0f);
        RX_INFO("nif: emissive pulse {:.2f}Hz amount {:.2f} {}", info.emissive_pulse[0],
                 info.emissive_pulse[1], source_path);
      }
    }
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
    if (shader && shader->water) {
      material.base_color_factor[0] = 0.08f;
      material.base_color_factor[1] = 0.12f;
      material.base_color_factor[2] = 0.16f;
      material.base_color_factor[3] = 0.75f;
      material.metallic_factor = 0;
      material.roughness_factor = 0.05f;
      material.alpha_mode = asset::AlphaMode::kBlend;
      material.two_sided = true;
      material.is_water = true;
    } else if (shader) {
      std::string diffuse, normal, height, glow;
      // Glow (type 2) or the glow-map flag routes texture slot 2 to the
      // emissive slot; the emissive color/multiple tints it.
      bool glow_mapped = !shader->effect &&
                         (shader->shader_type == 2 || (shader->flags2 & kShaderFlag2GlowMap));
      // Refraction shapes carry the distortion normal map in slot 0; it must
      // never bind as a diffuse. They go to the screen-space transmission path
      // instead (glass, ice, heat haze) with the background showing through.
      bool refraction =
          !shader->effect && !shader->water &&
          (shader->flags1 & (kShaderFlag1Refraction | kShaderFlag1FireRefraction)) != 0;
      if (shader->effect) {
        diffuse = NormalizeTexturePath(shader->effect_texture);
        material.alpha_mode = asset::AlphaMode::kBlend;
      } else if (ShaderTypeUsesDiffuseSlot(shader->shader_type)) {
        if (const auto* set = texture_sets.find(static_cast<u32>(shader->texture_set))) {
          if (set->size() > 0 && !refraction) diffuse = NormalizeTexturePath((*set)[0]);
          if (set->size() > 1) normal = NormalizeTexturePath((*set)[1]);
          // Parallax shader types carry a height map in slot 3 (the _p.dds
          // convention); it feeds the renderer's parallax occlusion march.
          // Multilayer parallax (11) repurposes the slot for the inner layer,
          // so only the plain (3) and occlusion (7) variants qualify.
          if ((shader->shader_type == 3 || shader->shader_type == 7) && set->size() > 3 &&
              !(*set)[3].empty()) {
            height = NormalizeTexturePath((*set)[3]);
            RX_INFO("nif: parallax height map {}", height);
          }
          if (glow_mapped && set->size() > 2 && !(*set)[2].empty()) {
            glow = NormalizeTexturePath((*set)[2]);
          }
        }
      }
      // River, creek and waterfall fx surfaces carry water tile textures on
      // plain lighting shaders; they belong to the water pipeline, not the
      // generic blend path.
      bool water_texture = diffuse.find("fxwater") != std::string::npos ||
                           diffuse.find("watertile") != std::string::npos ||
                           diffuse.find("fxrapids") != std::string::npos ||
                           diffuse.find("defaultwater") != std::string::npos;
      // Foam overlays (whitewater, waterfall strips) stay on the blend path.
      if (diffuse.find("whitewater") != std::string::npos ||
          diffuse.find("waterfall") != std::string::npos) {
        water_texture = false;
      }
      if (water_texture) {
        material.base_color_factor[0] = 0.08f;
        material.base_color_factor[1] = 0.12f;
        material.base_color_factor[2] = 0.16f;
        material.base_color_factor[3] = 0.75f;
        material.metallic_factor = 0;
        material.roughness_factor = 0.05f;
        material.alpha_mode = asset::AlphaMode::kBlend;
        material.two_sided = true;
        material.is_water = true;
        diffuse.clear();
        normal.clear();
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
      if (!height.empty()) {
        material.height = asset::MakeAssetId(height);
        // Skyrim stores no per-material depth; ~3 cm at typical 2 m texture
        // tiling reads right for stone and timber without silhouette breakup.
        material.height_scale = 0.03f;
        if (std::ranges::find(result.texture_paths, height) == result.texture_paths.end()) {
          result.texture_paths.push_back(std::move(height));
        }
      }
      if (!glow.empty()) {
        material.emissive = asset::MakeAssetId(glow);
        if (std::ranges::find(result.texture_paths, glow) == result.texture_paths.end()) {
          result.texture_paths.push_back(std::move(glow));
        }
      }
      material.two_sided = (shader->flags2 & 0x10) != 0;
      bool env_mapped = !shader->effect &&
                        (shader->shader_type == 1 || shader->shader_type == 16 ||
                         (shader->flags1 & (kShaderFlag1EnvMapping | kShaderFlag1EyeEnvMapping)));
      material.metallic_factor = 0;
      // Specular power to perceptual roughness, Karis' approximation. Env-mapped
      // surfaces relax the matte floor so the engine's IBL/RT reflections stand
      // in for the (unsupported) authored cubemap.
      f32 gloss = std::clamp(shader->glossiness, 1.0f, 1000.0f);
      f32 rough_floor = env_mapped ? 0.05f : 0.2f;
      material.roughness_factor =
          std::clamp(std::pow(2.0f / (gloss + 2.0f), 0.25f), rough_floor, 1.0f);
      if (env_mapped) {
        // Environment Map Scale (parsed for type 1) drives reflectivity through
        // metallic, since most env-mapped Skyrim surfaces are metal; cap
        // roughness so the reflection reads.
        f32 scale = shader->env_map_scale >= 0.0f ? shader->env_map_scale : 0.6f;
        material.metallic_factor = std::clamp(scale, 0.1f, 1.0f);
        material.roughness_factor = std::min(material.roughness_factor, 0.35f);
      }
      // Emissive: own-emit fills a uniform emittance; a bound glow map modulates
      // the emissive-slot texture. A near-black glow color with a glow map means
      // "use the map as-is", so it glows white rather than not at all.
      bool own_emit = (shader->flags1 & kShaderFlag1OwnEmit) != 0;
      bool glow_tex = static_cast<bool>(material.emissive);
      if (!shader->effect && (own_emit || glow_tex)) {
        f32 tint[3] = {shader->emissive[0], shader->emissive[1], shader->emissive[2]};
        if (glow_tex && tint[0] + tint[1] + tint[2] < 1e-3f) {
          tint[0] = tint[1] = tint[2] = 1.0f;
        }
        for (int k = 0; k < 3; ++k) {
          material.emissive_factor[k] = tint[k] * shader->emissive_multiple;
        }
        material.emissive_pulse[0] = shader->emissive_pulse[0];
        material.emissive_pulse[1] = shader->emissive_pulse[1];
      }
      // Material-class flags from the shader type: skin (screen-space sss plus a
      // reddish analytic transmit), hair (Kajiya-Kay strands), tree/foliage wind.
      if (shader->shader_type == 4 || shader->shader_type == 5) {
        material.skin = true;
        material.subsurface = 0.3f;  // subsurface_color default is reddish
      } else if (shader->shader_type == 6) {
        material.hair = true;
        material.subsurface = 0.15f;
      }
      if (shader->shader_type == 12 || (shader->flags2 & kShaderFlag2TreeAnim)) {
        material.wind = true;
      }
      if (refraction) {
        // Mostly-transmissive regardless of the authored strength (which only
        // scales the screen distortion): the base color stays white so the
        // refracted background shows untinted, and transmission forces the
        // blend path in the material system.
        material.transmission = std::clamp(0.5f + shader->refraction_strength, 0.5f, 1.0f);
        material.alpha_mode = asset::AlphaMode::kBlend;
      }
      material.uv_scroll_u = shader->uv_scroll_u;
      material.uv_scroll_v = shader->uv_scroll_v;
    }
    if (const AlphaInfo* alpha = alphas.find(static_cast<u32>(alpha_block))) {
      if (alpha->flags & 0x0001) {
        material.alpha_mode = asset::AlphaMode::kBlend;
      } else if (alpha->flags & 0x0200) {
        material.alpha_mode = asset::AlphaMode::kMask;
        material.alpha_cutoff = static_cast<f32>(alpha->threshold) / 255.0f;
      }
    }

    // Effect-shader geometry: unlit emissive transparent. base_color is the
    // source texture (bound above); base_color_factor carries the emissive
    // colour * multiple, additive vs alpha comes from the NiAlphaProperty, and
    // the greyscale palette (when used) binds into the emissive slot.
    if (shader && shader->effect) {
      material.effect = true;
      material.alpha_mode = asset::AlphaMode::kBlend;
      const AlphaInfo* alpha = alphas.find(static_cast<u32>(alpha_block));
      material.effect_additive =
          alpha && (alpha->flags & 0x0001) && ((alpha->flags >> 5) & 0xf) == 0;
      material.effect_grayscale_color = (shader->flags1 & kShaderFlag1GrayscaleToColor) != 0;
      material.effect_grayscale_alpha = (shader->flags1 & kShaderFlag1GrayscaleToAlpha) != 0;
      material.effect_falloff = (shader->flags1 & kShaderFlag1UseFalloff) != 0;
      for (int k = 0; k < 4; ++k) material.effect_falloff_params[k] = shader->falloff[k];
      for (int k = 0; k < 3; ++k) {
        material.base_color_factor[k] = shader->effect_color[k] * shader->effect_scale;
      }
      material.base_color_factor[3] = shader->effect_color[3];
      material.two_sided = (shader->flags2 & kShaderFlag2DoubleSided) != 0;
      material.emissive_pulse[0] = shader->emissive_pulse[0];
      material.emissive_pulse[1] = shader->emissive_pulse[1];
      if (material.effect_grayscale_color && !shader->greyscale_texture.empty()) {
        std::string palette = NormalizeTexturePath(shader->greyscale_texture);
        material.emissive = asset::MakeAssetId(palette);
        if (std::ranges::find(result.texture_paths, palette) == result.texture_paths.end()) {
          result.texture_paths.push_back(std::move(palette));
        }
      }
    }

    result.materials.push_back(material);
    result.material_files.push_back(shader ? shader->material_file : std::string());
    material_ids.emplace(key, material.id);
    return material.id;
  };

  // World transform per node block, for skinned shapes whose bones live in
  // other subtrees than the shape itself.
  base::UnorderedMap<u32, Transform> node_world;
  {
    struct NodeEntry {
      u32 block;
      Transform parent;
    };
    base::Vector<NodeEntry> walk;
    for (u32 root : roots) walk.push_back({root, Transform{}});
    while (!walk.empty()) {
      NodeEntry entry = walk.back();
      walk.pop_back();
      const Node* node = nodes.find(entry.block);
      if (!node || node_world.contains(entry.block)) continue;
      Transform world = Compose(entry.parent, node->local);
      node_world.emplace(entry.block, world);
      for (i32 child : node->children) {
        if (child >= 0) walk.push_back({static_cast<u32>(child), world});
      }
    }
  }

  // Bakes a skinned shape rigidly at its bind pose. False when the skeleton
  // is not fully contained in this file (actor parts).
  auto bake_skinned = [&](const Shape& shape, Geometry* out) -> bool {
    const SkinInstanceBlock* skin = skin_instances.find(static_cast<u32>(shape.skin));
    if (!skin) return false;
    const base::Vector<Transform>* skin_to_bone = skin_datas.find(static_cast<u32>(skin->data));
    const SkinPartitionBlock* partition = skin_partitions.find(static_cast<u32>(skin->partition));
    if (!skin_to_bone || !partition || skin_to_bone->size() != skin->bones.size()) return false;
    if (partition->vertices.empty() || partition->indices.empty()) return false;

    base::Vector<Transform> bone_world;
    bone_world.reserve(skin->bones.size());
    for (size_t b = 0; b < skin->bones.size(); ++b) {
      i32 bone = skin->bones[b];
      const Transform* world = bone >= 0 ? node_world.find(static_cast<u32>(bone)) : nullptr;
      if (!world) return false;  // external skeleton
      bone_world.push_back(Compose(*world, (*skin_to_bone)[b]));
    }

    out->vertices = partition->vertices;
    out->indices = partition->indices;
    // Vertex bone indices are partition local; resolve them through each
    // partition's palette by walking its triangles.
    base::Vector<u32> global_bones(partition->vertices.size() * 4);
    base::Vector<u8> resolved(partition->vertices.size());
    for (const SkinPartitionBlock::Span& span : partition->spans) {
      for (u32 k = 0; k < span.index_count; ++k) {
        u32 v = partition->indices[span.first_index + k];
        if (resolved[v]) continue;
        resolved[v] = 1;
        for (u32 w = 0; w < 4; ++w) {
          u8 local = partition->skin.bone_indices[v * 4 + w];
          global_bones[v * 4 + w] =
              local < span.bones.size() ? span.bones[local] : 0;
        }
      }
    }
    for (size_t v = 0; v < out->vertices.size(); ++v) {
      f32 total = 0;
      f32 position[3] = {0, 0, 0};
      f32 normal[3] = {0, 0, 0};
      f32 tangent[3] = {0, 0, 0};
      const asset::Vertex& src = partition->vertices[v];
      for (u32 w = 0; w < 4; ++w) {
        f32 weight = partition->skin.weights[v * 4 + w];
        if (weight <= 0) continue;
        u32 bone = global_bones[v * 4 + w];
        if (bone >= bone_world.size()) continue;
        const Transform& t = bone_world[bone];
        f32 p[3], n[3], tg[3];
        t.Apply(src.position, p);
        t.Rotate(src.normal, n);
        t.Rotate(src.tangent, tg);
        for (int k = 0; k < 3; ++k) {
          position[k] += weight * p[k];
          normal[k] += weight * n[k];
          tangent[k] += weight * tg[k];
        }
        total += weight;
      }
      asset::Vertex& dst = out->vertices[v];
      if (total > 1e-4f) {
        f32 inv = 1.0f / total;
        for (int k = 0; k < 3; ++k) {
          dst.position[k] = position[k] * inv;
          dst.normal[k] = normal[k] * inv;
          dst.tangent[k] = tangent[k] * inv;
        }
      }
    }
    return true;
  };

  // Keeps a skinned shape for runtime GPU skinning instead of baking: vertices
  // stay in bind space, per-vertex bone indices/weights are emitted, and the
  // bones are merged (by name) into mesh->skin so a skeleton can drive them.
  base::UnorderedMap<u64, u32> skin_bone_lookup;  // name hash -> mesh->skin index
  auto name_hash = [](const std::string& s) -> u64 {
    u64 h = 1469598103934665603ull;
    for (char c : s) {
      h ^= static_cast<u8>(c);
      h *= 1099511628211ull;
    }
    return h;
  };
  auto emit_runtime_skin = [&](const Shape& shape, Geometry* out,
                               base::Vector<asset::SkinnedVertexExtra>* out_skin) -> bool {
    const SkinInstanceBlock* skin = skin_instances.find(static_cast<u32>(shape.skin));
    if (!skin) return false;
    const base::Vector<Transform>* skin_to_bone = skin_datas.find(static_cast<u32>(skin->data));
    const SkinPartitionBlock* partition = skin_partitions.find(static_cast<u32>(skin->partition));
    if (!skin_to_bone || !partition || skin_to_bone->size() != skin->bones.size()) return false;
    if (partition->vertices.empty() || partition->indices.empty()) return false;

    // Map this shape's skin bones into mesh->skin by node name (dismembered
    // bodies split one skeleton across several partitions).
    base::Vector<u32> bone_remap(skin->bones.size());
    for (size_t b = 0; b < skin->bones.size(); ++b) {
      i32 bone_block = skin->bones[b];
      const Node* bone_node = bone_block >= 0 ? nodes.find(static_cast<u32>(bone_block)) : nullptr;
      std::string bone_name = bone_node ? bone_node->name : std::string();
      if (bone_name.empty()) bone_name = "Bone" + std::to_string(bone_block);
      u64 h = name_hash(bone_name);
      if (u32* known = skin_bone_lookup.find(h)) {
        bone_remap[b] = *known;
      } else {
        u32 idx = static_cast<u32>(mesh->skin.bones.size());
        mesh->skin.bones.push_back(bone_name);
        mesh->skin.inverse_bind.push_back(ToMat4((*skin_to_bone)[b]));
        skin_bone_lookup.emplace(h, idx);
        bone_remap[b] = idx;
      }
    }

    out->vertices = partition->vertices;
    out->indices = partition->indices;
    out_skin->resize(partition->vertices.size());
    base::Vector<u8> resolved(partition->vertices.size());
    for (const SkinPartitionBlock::Span& span : partition->spans) {
      for (u32 k = 0; k < span.index_count; ++k) {
        u32 v = partition->indices[span.first_index + k];
        if (resolved[v]) continue;
        resolved[v] = 1;
        f32 w[4];
        f32 total = 0;
        for (u32 j = 0; j < 4; ++j) {
          w[j] = partition->skin.weights[v * 4 + j];
          total += w[j];
        }
        asset::SkinnedVertexExtra& extra = (*out_skin)[v];
        for (u32 j = 0; j < 4; ++j) {
          u8 local = partition->skin.bone_indices[v * 4 + j];
          u32 global = local < span.bones.size() ? span.bones[local] : 0;  // skin->bones index
          extra.bone_indices[j] =
              global < bone_remap.size() ? static_cast<u8>(bone_remap[global]) : 0;
          f32 norm = total > 1e-5f ? w[j] / total : (j == 0 ? 1.0f : 0.0f);
          extra.bone_weights[j] = static_cast<u8>(std::lround(norm * 255.0f));
        }
      }
    }
    return true;
  };

  // Folds one particle system's modifier list into asset emitters on the mesh,
  // in mesh-local (NIF object) space; the instance transform maps them to
  // engine world space at draw time. The billboard renderer has no texture
  // sampling, so the well-known effect classes read through color instead.
  auto emit_psys = [&](u32 block, const PsysBlock& psys, const Transform& psys_world) {
    // Birth rate: the NiPSysEmitterCtlr targeting this system drives a float
    // interpolator whose keys are births/sec.
    f32 rate = -1;
    for (const EmitterCtlrBlock& ctlr : emitter_ctlrs) {
      if (ctlr.target != static_cast<i32>(block) || ctlr.interpolator < 0) continue;
      const FloatInterp* interp = interp_data.find(static_cast<u32>(ctlr.interpolator));
      if (!interp) continue;
      const FloatKeys* keys =
          interp->data >= 0 ? float_datas.find(static_cast<u32>(interp->data)) : nullptr;
      // Keyed birth rate or the interpolator's constant pose value.
      rate = keys ? std::max(keys->first_value, keys->last_value) : interp->pose;
    }
    const ShaderInfo* shader = shaders.find(static_cast<u32>(psys.shader));
    const AlphaInfo* alpha = alphas.find(static_cast<u32>(psys.alpha));
    // Additive blend (fire, glows): destination factor ONE in the alpha flags.
    bool additive = alpha && (alpha->flags & 0x0001) && ((alpha->flags >> 5) & 0xf) == 0;
    std::string texture;
    if (shader) {
      if (shader->effect) {
        texture = NormalizeTexturePath(shader->effect_texture);
      } else if (const auto* set = texture_sets.find(static_cast<u32>(shader->texture_set))) {
        if (set->size() > 0) texture = NormalizeTexturePath((*set)[0]);
      }
    }
    const PsysData* psys_data =
        psys.data >= 0 ? psys_max.find(static_cast<u32>(psys.data)) : nullptr;
    // Bind the system's texture on the billboards (loaded + bindless-registered
    // via texture_paths; the renderer resolves the hash to a bindless index).
    u64 texture_hash = 0;
    if (!texture.empty()) {
      texture_hash = asset::MakeAssetId(texture).hash;
      if (std::ranges::find(result.texture_paths, texture) == result.texture_paths.end()) {
        result.texture_paths.push_back(texture);
      }
    }
    // BSPSysSimpleColorModifier tints/fades the particles over their life.
    const ColorModBlock* color_mod = nullptr;
    for (i32 mod : psys.modifiers) {
      if (mod >= 0) {
        if (const ColorModBlock* c = color_mods.find(static_cast<u32>(mod))) {
          color_mod = c;
          break;
        }
      }
    }
    RX_DEBUG("nif: psys tex='{}' additive={} rate={} alpha_flags={:#x} {}", texture, additive,
              rate, alpha ? alpha->flags : 0u, source_path);
    bool known_class = texture.find("fire") != std::string::npos ||
                       texture.find("flame") != std::string::npos ||
                       texture.find("ember") != std::string::npos ||
                       texture.find("candle") != std::string::npos ||
                       texture.find("smoke") != std::string::npos ||
                       texture.find("mist") != std::string::npos ||
                       texture.find("steam") != std::string::npos ||
                       texture.find("fog") != std::string::npos;
    // No birth-rate controller and not a known ambient class: likely an
    // event-driven system (destruction splinters); a constant default rate
    // would emit phantoms forever.
    if (rate <= 0 && !known_class) return;

    for (i32 mod : psys.modifiers) {
      const EmitterBlock* e = mod >= 0 ? emitter_blocks.find(static_cast<u32>(mod)) : nullptr;
      if (!e) continue;
      const Transform* obj_world =
          e->emitter_object >= 0 ? node_world.find(static_cast<u32>(e->emitter_object)) : nullptr;
      const Transform& world = obj_world ? *obj_world : psys_world;

      asset::ParticleEmitter out;
      for (int k = 0; k < 3; ++k) out.position[k] = world.t[k];
      f32 emit_axis[3] = {0, 0, 1};  // Gamebryo births along +z before spread
      f32 dir[3];
      world.Rotate(emit_axis, dir);
      for (int k = 0; k < 3; ++k) out.velocity[k] = dir[k] * e->speed * world.s;
      for (int k = 0; k < 3; ++k) out.extent[k] = e->half_extent[k] * world.s;
      // NiPSysMeshEmitter (fire burning along a log): spread births over the
      // referenced mesh's bounding volume (MVP for per-vertex surface emission).
      if (e->is_mesh) {
        f32 lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        bool got = false;
        for (i32 ref : e->mesh_refs) {
          if (ref < 0) continue;
          const Geometry* g = nullptr;
          if (const Shape* s = shapes.find(static_cast<u32>(ref))) {
            if (!s->geometry.vertices.empty()) g = &s->geometry;
            else if (s->data >= 0) g = geometry_blocks.find(static_cast<u32>(s->data));
          }
          if (!g) continue;
          for (const asset::Vertex& v : g->vertices) {
            for (int k = 0; k < 3; ++k) {
              lo[k] = std::min(lo[k], v.position[k]);
              hi[k] = std::max(hi[k], v.position[k]);
            }
          }
          got = true;
        }
        if (got) {
          for (int k = 0; k < 3; ++k) out.extent[k] = (hi[k] - lo[k]) * 0.5f * world.s;
        }
      }
      out.spread = std::clamp(e->declination_variation, 0.0f, 3.14159f);
      out.speed_variation = e->speed_variation * world.s;
      out.life = std::max(e->life, 0.05f);
      out.life_variation = e->life_variation;
      out.size = std::max(e->radius * world.s, 0.5f);
      for (int k = 0; k < 4; ++k) out.color[k] = e->color[k];
      if (rate > 0) out.rate = std::clamp(rate, 0.1f, 200.0f);
      if (psys_data && psys_data->max_particles > 0) {
        out.max_particles = std::min<u32>(psys_data->max_particles, 128);
      }
      if (psys_data && psys_data->frames > 1) {
        out.subtex_frames = psys_data->frames;
        out.subtex_cols = psys_data->cols;
        out.subtex_rows = psys_data->rows;
        RX_INFO("vfx: flipbook {}x{} ({} frames) '{}' {}", psys_data->cols, psys_data->rows,
                 psys_data->frames, texture, source_path);
      }
      for (i32 gm : psys.modifiers) {
        const GravityBlock* g = gm >= 0 ? gravity_blocks.find(static_cast<u32>(gm)) : nullptr;
        if (!g) continue;
        const Transform* gravity_world =
            g->object >= 0 ? node_world.find(static_cast<u32>(g->object)) : nullptr;
        f32 gravity_axis[3];
        (gravity_world ? *gravity_world : psys_world).Rotate(g->axis, gravity_axis);
        for (int k = 0; k < 3; ++k) out.gravity[k] += gravity_axis[k] * g->strength * world.s;
      }
      out.additive = additive;
      if (texture.find("fire") != std::string::npos ||
          texture.find("flame") != std::string::npos ||
          texture.find("ember") != std::string::npos ||
          texture.find("candle") != std::string::npos) {
        out.additive = true;  // hdr radiance, bloom does the glow
        out.color[0] = 2.5f;
        out.color[1] = 1.0f;
        out.color[2] = 0.3f;
        out.color[3] = 1.0f;
      } else if (texture.find("smoke") != std::string::npos) {
        out.additive = false;
        out.color[0] = out.color[1] = out.color[2] = 0.05f;
        out.color[3] = 0.4f;
      } else if (texture.find("mist") != std::string::npos ||
                 texture.find("steam") != std::string::npos ||
                 texture.find("fog") != std::string::npos) {
        out.additive = false;
        out.color[0] = 0.6f;
        out.color[1] = 0.65f;
        out.color[2] = 0.7f;
        out.color[3] = 0.12f;
      }
      out.texture = texture_hash;
      if (color_mod) {
        out.has_color_ramp = true;
        for (int j = 0; j < 6; ++j) out.ramp_key[j] = color_mod->key[j];
        for (int j = 0; j < 3; ++j) {
          for (int k = 0; k < 4; ++k) out.ramp_color[j][k] = color_mod->color[j][k];
        }
      }
      mesh->emitters.push_back(out);
    }
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

    if (const PsysBlock* psys = psys_blocks.find(entry.block)) {
      if (!psys->hidden && !keep_skin && NifParticles.get()) {
        emit_psys(entry.block, *psys, Compose(entry.world, psys->local));
      }
      continue;
    }

    Shape* shape = shapes.find(entry.block);
    if (!shape || shape->hidden) continue;
    const ShaderInfo* shader = shaders.find(static_cast<u32>(shape->shader));
    // Effect-shader geometry (torch/campfire flames, glow planes, god rays, mist
    // sheets) renders through the unlit emissive blend path. FO4/FO76 effect
    // properties (bs >= 130) parse a different tail, so keep skipping those.
    // Refraction shapes render through the transmission path (material_for never
    // binds their distortion map as diffuse); the knob restores the skip.
    bool effect_shape = shader && shader->effect;
    // Editor / furniture-use markers (XMarker, CivilWar, cart/idle furniture,
    // weapon markers) carry effect planes that are invisible in-game: they are
    // named "*marker*" and, tellingly, bind no source texture (nothing to draw),
    // so both signals gate them out while real vfx keep their textured planes.
    bool marker = source_path.find("marker") != std::string_view::npos ||
                  (shader && shader->effect && shader->effect_texture.empty());
    bool render_effect = effect_shape && NifEffectShaders.get() && header->bs_version < 130 &&
                         !keep_skin && !marker;
    bool refraction_shape =
        shader && !shader->effect && !shader->water &&
        (shader->flags1 & (kShaderFlag1Refraction | kShaderFlag1FireRefraction)) != 0;
    if (shader && ((effect_shape && !render_effect) ||
                   (refraction_shape && !NifRefraction.get()))) {
      ++result.skipped_shapes;
      continue;
    }
    if (refraction_shape) ++result.refraction_shapes;
    if (render_effect) ++result.effect_shapes;
    bool skinned = false;
    Geometry baked;
    base::Vector<asset::SkinnedVertexExtra> baked_skin;  // keep_skin only
    const Geometry* geometry = &shape->geometry;
    // BSDynamicTriShape (head, hair): dynamic positions are inline, the
    // triangles live in the skin partition. Stitch them for the rigid path.
    if (rigid_fallback && shape->skin >= 0 && !shape->geometry.vertices.empty() &&
        shape->geometry.indices.empty()) {
      const SkinInstanceBlock* skin = skin_instances.find(static_cast<u32>(shape->skin));
      const SkinPartitionBlock* part =
          skin ? skin_partitions.find(static_cast<u32>(skin->partition)) : nullptr;
      if (part && !part->indices.empty()) {
        baked = shape->geometry;
        baked.indices = part->indices;
        // Dynamic shapes only carry positions inline; the partition buffer holds
        // the matching UV/normal/tangent/color (same global vertex order).
        if (part->vertices.size() == baked.vertices.size()) {
          for (size_t vi = 0; vi < baked.vertices.size(); ++vi) {
            const asset::Vertex& src = part->vertices[vi];
            asset::Vertex& dst = baked.vertices[vi];
            dst.uv[0] = src.uv[0];
            dst.uv[1] = src.uv[1];
            std::memcpy(dst.normal, src.normal, sizeof(dst.normal));
            std::memcpy(dst.tangent, src.tangent, sizeof(dst.tangent));
            dst.color = src.color;
          }
        }
        bool valid = true;
        for (u32 idx : baked.indices) {
          if (idx >= baked.vertices.size()) {
            valid = false;
            break;
          }
        }
        if (valid) geometry = &baked;
      }
    }
    if (shape->skin >= 0 && geometry->vertices.empty()) {
      bool ok = keep_skin ? emit_runtime_skin(*shape, &baked, &baked_skin)
                          : bake_skinned(*shape, &baked);
      if (ok) {
        geometry = &baked;
        skinned = true;
      } else if (rigid_fallback) {
        // External skeleton (head, hair): take the un-posed bind geometry so
        // the caller can rigid-attach it to a bone.
        if (shape->data >= 0) {
          geometry = geometry_blocks.find(static_cast<u32>(shape->data));
        } else {
          const SkinInstanceBlock* skin = skin_instances.find(static_cast<u32>(shape->skin));
          const SkinPartitionBlock* part =
              skin ? skin_partitions.find(static_cast<u32>(skin->partition)) : nullptr;
          if (part && !part->vertices.empty()) {
            baked.vertices = part->vertices;
            baked.indices = part->indices;
            geometry = &baked;
          } else {
            geometry = nullptr;
          }
        }
        if (!geometry || geometry->vertices.empty()) shape->skipped = true;
      } else {
        shape->skipped = true;
      }
    } else if (shape->data >= 0) {
      geometry = geometry_blocks.find(static_cast<u32>(shape->data));
      if (!geometry) shape->skipped = true;
    }
    // A skinned mesh only carries skinned shapes; a stray static decal would
    // have no bone to follow, so drop it.
    if (keep_skin && !skinned) {
      ++result.skipped_shapes;
      continue;
    }
    if (shape->skipped || !geometry || geometry->vertices.empty() || geometry->indices.empty()) {
      if (shape->skipped) ++result.skipped_shapes;
      continue;
    }

    // Skinned geometry stays in bind/skeleton space; baked geometry is already
    // in scene space via its bone transforms.
    Transform world = skinned ? Transform{} : Compose(entry.world, shape->local);
    u32 vertex_base = static_cast<u32>(lod.vertices.size());
    u32 index_offset = static_cast<u32>(lod.indices.size());
    for (size_t vi = 0; vi < geometry->vertices.size(); ++vi) {
      const asset::Vertex& src = geometry->vertices[vi];
      asset::Vertex v = src;
      // Vertex alpha stores wind weights on foliage and must not feed the alpha
      // test, so it is forced opaque everywhere except effect-shader planes,
      // which fade the authored flame/glow through it.
      if (!render_effect) v.color |= 0xff000000u;
      world.Apply(src.position, v.position);
      world.Rotate(src.normal, v.normal);
      world.Rotate(src.tangent, v.tangent);
      for (int k = 0; k < 3; ++k) {
        bounds_min[k] = std::min(bounds_min[k], v.position[k]);
        bounds_max[k] = std::max(bounds_max[k], v.position[k]);
      }
      lod.vertices.push_back(v);
      if (keep_skin && skinned) lod.skinning.push_back(baked_skin[vi]);
    }
    for (u32 index : geometry->indices) lod.indices.push_back(vertex_base + index);
    if (skinned) ++result.skinned_shapes;

    asset::Submesh submesh;
    submesh.index_offset = index_offset;
    submesh.index_count = static_cast<u32>(geometry->indices.size());
    submesh.material = material_for(shape->shader, shape->alpha);
    lod.submeshes.push_back(submesh);
  }

  // Torch/campfire flame bodies are now real BSEffectShaderProperty planes
  // (rendered above), so the old synthesized additive-flame hack is gone; only
  // the authored smoke/ember particle systems remain.

  // Emitter-only NIFs (smoke columns, dust wisps: particle systems with no
  // geometry) carry no vertices but must still spawn their pools, so keep the
  // mesh instead of nulling it. The renderer registers its emitters and skips
  // the (empty) geometry.
  if (lod.vertices.empty() && mesh->emitters.empty()) {
    result.mesh = nullptr;
    return result;
  }

  if (lod.vertices.empty()) {
    // Emitter-only mesh: no geometry to bound, so center on the first emitter.
    if (!mesh->emitters.empty()) {
      for (int k = 0; k < 3; ++k) mesh->bounds_center[k] = mesh->emitters[0].position[k];
    }
    mesh->bounds_radius = 0;
  } else {
    for (int k = 0; k < 3; ++k) mesh->bounds_center[k] = (bounds_min[k] + bounds_max[k]) * 0.5f;
    f32 radius_sq = 0;
    for (int k = 0; k < 3; ++k) {
      f32 d = bounds_max[k] - mesh->bounds_center[k];
      radius_sq += d * d;
    }
    mesh->bounds_radius = std::sqrt(radius_sq);
  }
  if (keep_skin && !mesh->skin.bones.empty()) {
    mesh->skinned = true;
    result.skinned = true;
    if (mesh->skin.bones.size() > 256) {
      RX_WARN("nif {} skins {} bones, over the 256 GPU palette limit",
               source_path, mesh->skin.bones.size());
    }
  }
  result.mesh = std::move(mesh);
  return result;
}

NifConversion ConvertNifScene(ByteSpan data, asset::AssetId id, std::string_view source_path) {
  return ConvertNifImpl(data, id, source_path, /*keep_skin=*/false, /*rigid_fallback=*/false);
}

NifConversion ConvertNifSkinnedMesh(ByteSpan data, asset::AssetId id,
                                    std::string_view source_path) {
  return ConvertNifImpl(data, id, source_path, /*keep_skin=*/true, /*rigid_fallback=*/false);
}

NifConversion ConvertNifRigid(ByteSpan data, asset::AssetId id, std::string_view source_path) {
  return ConvertNifImpl(data, id, source_path, /*keep_skin=*/false, /*rigid_fallback=*/true);
}

bool ConvertNifSkeleton(ByteSpan data, asset::AssetId id, asset::Skeleton* out) {
  auto header = ParseNifHeader(data);
  if (!header) return false;
  u32 block_count = static_cast<u32>(header->block_sizes.size());

  // Parse every NiNode: its name, local bind, and child block refs.
  struct RawNode {
    std::string name;
    Transform local;
    base::Vector<i32> children;
  };
  base::UnorderedMap<u32, RawNode> raw;
  for (u32 i = 0; i < block_count; ++i) {
    const std::string& type = header->block_types[header->block_type_index[i]];
    if (!type.ends_with("Node")) continue;
    Reader r{data.subspan(header->block_offsets[i], header->block_sizes[i])};
    RawNode node;
    i32 name_index = -1;
    node.local = ReadAvObject(r, nullptr, &name_index);
    if (name_index >= 0 && static_cast<u32>(name_index) < header->strings.size()) {
      node.name = header->strings[name_index];
    }
    u32 child_count = r.Read<u32>();
    if (!r.ok || child_count > 65536) continue;
    for (u32 c = 0; c < child_count; ++c) node.children.push_back(r.Read<i32>());
    if (r.ok) raw.emplace(i, std::move(node));
  }
  if (raw.empty()) return false;

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
        if (r.ok && root >= 0 && raw.find(static_cast<u32>(root))) {
          roots.push_back(static_cast<u32>(root));
        }
      }
    }
    if (roots.empty()) roots.push_back(0);
  }

  // Depth first so parents always land in `bones` before their children. The
  // block index of a bone maps to its final array index.
  out->bones.clear();
  out->id = id;
  base::UnorderedMap<u32, i32> block_to_bone;
  struct Entry {
    u32 block;
    i32 parent;
  };
  base::Vector<Entry> stack;
  for (u32 root : roots) stack.push_back({root, -1});
  while (!stack.empty()) {
    Entry entry = stack.back();
    stack.pop_back();
    const RawNode* node = raw.find(entry.block);
    if (!node || block_to_bone.find(entry.block)) continue;
    i32 index = static_cast<i32>(out->bones.size());
    block_to_bone.emplace(entry.block, index);
    asset::Bone bone;
    bone.name = node->name;
    bone.parent = entry.parent;
    bone.bind_translation = {node->local.t[0], node->local.t[1], node->local.t[2]};
    bone.bind_rotation = QuatFromTransform(node->local);
    bone.bind_scale = node->local.s;
    out->bones.push_back(std::move(bone));
    for (i32 child : node->children) {
      if (child >= 0) stack.push_back({static_cast<u32>(child), index});
    }
  }
  return !out->bones.empty();
}

}  // namespace rx::bethesda
