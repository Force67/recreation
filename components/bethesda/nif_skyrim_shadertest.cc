// nif_skyrim_shadertest: the Skyrim BSLightingShaderProperty tail is read by
// offset, not by name, so a single wrong field width silently mis-shades every
// mesh in the game (the specular strength lands on the rim power, the
// environment scale on nothing). A hand-built Skyrim NIF (BS stream 100) with
// one env-map shader property pins those offsets: the values that come out the
// far end as an asset::Material are the ones written here.
//
// No game data needed, so it runs in the ctest gate.

#include <base/containers/vector.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "asset/asset_id.h"
#include "components/bethesda/nif.h"
#include "core/types.h"

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    ++g_failures;
}

void CheckNear(const char* what, rx::f32 got, rx::f32 want) {
  bool ok = std::fabs(got - want) < 1e-4f;
  std::printf("  [%s] %s (got %.3f, want %.3f)\n", ok ? "ok" : "FAIL", what, got, want);
  if (!ok)
    ++g_failures;
}

void PutU8(base::Vector<rx::u8>& b, rx::u8 v) {
  b.push_back(v);
}
void PutU16(base::Vector<rx::u8>& b, rx::u16 v) {
  b.push_back(rx::u8(v));
  b.push_back(rx::u8(v >> 8));
}
void PutU32(base::Vector<rx::u8>& b, rx::u32 v) {
  for (int i = 0; i < 4; ++i)
    b.push_back(rx::u8(v >> (8 * i)));
}
void PutI32(base::Vector<rx::u8>& b, rx::i32 v) {
  PutU32(b, static_cast<rx::u32>(v));
}
void PutU64(base::Vector<rx::u8>& b, rx::u64 v) {
  for (int i = 0; i < 8; ++i)
    b.push_back(rx::u8(v >> (8 * i)));
}
void PutF32(base::Vector<rx::u8>& b, float f) {
  rx::u32 v;
  std::memcpy(&v, &f, 4);
  PutU32(b, v);
}
void PutSizedStr(base::Vector<rx::u8>& b, const char* s) {  // u32 len + bytes
  rx::u32 n = 0;
  while (s[n])
    ++n;
  PutU32(b, n);
  for (rx::u32 i = 0; i < n; ++i)
    b.push_back(static_cast<rx::u8>(s[i]));
}

void PutAvObject(base::Vector<rx::u8>& b) {
  PutI32(b, -1);  // name
  PutU32(b, 0);   // extra data count
  PutI32(b, -1);  // controller
  PutU32(b, 0);   // flags
  for (int i = 0; i < 3; ++i)
    PutF32(b, 0.0f);
  const float ident[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  for (float v : ident)
    PutF32(b, v);
  PutF32(b, 1.0f);  // scale
  PutI32(b, -1);    // collision object
}

// Written values, all distinct so a shifted read shows up as the wrong number
// rather than a plausible one.
constexpr rx::u32 kShaderTypeEnvMap = 1;
constexpr rx::u32 kFlags1Specular = 1u << 0;
constexpr rx::u32 kFlags2SoftLighting = 1u << 25;
constexpr rx::u32 kFlags2RimLighting = 1u << 26;
constexpr float kGlossiness = 500.0f;
constexpr float kSpecularColor[3] = {0.75f, 0.5f, 0.25f};
constexpr float kSpecularStrength = 2.0f;
constexpr float kSoftLighting = 0.3f;
constexpr float kRimPower = 4.0f;
constexpr float kEnvMapScale = 0.8f;

// BSLightingShaderProperty, Skyrim (BS 100) layout as the reader walks it.
void PutShaderProperty(base::Vector<rx::u8>& b, rx::i32 texture_set_block) {
  PutU32(b, kShaderTypeEnvMap);
  PutI32(b, -1);  // name (FO4 material file; unused here)
  PutU32(b, 0);   // extra data count
  PutI32(b, -1);  // controller
  PutU32(b, kFlags1Specular);
  PutU32(b, kFlags2SoftLighting | kFlags2RimLighting);
  PutF32(b, 0.0f);  // uv offset u
  PutF32(b, 0.0f);  // uv offset v
  PutF32(b, 1.0f);  // uv scale u
  PutF32(b, 1.0f);  // uv scale v
  PutI32(b, texture_set_block);
  for (int i = 0; i < 3; ++i)
    PutF32(b, 0.0f);  // emissive color
  PutF32(b, 1.0f);    // emissive multiple
  PutU32(b, 0);       // texture clamp mode
  PutF32(b, 1.0f);    // alpha
  PutF32(b, 0.0f);    // refraction strength
  PutF32(b, kGlossiness);
  for (float v : kSpecularColor)
    PutF32(b, v);
  PutF32(b, kSpecularStrength);
  PutF32(b, kSoftLighting);  // lighting effect 1
  PutF32(b, kRimPower);      // lighting effect 2
  PutF32(b, kEnvMapScale);   // environment map scale (shader type 1 tail)
}

// The nine Skyrim texture slots. Only the ones the converter binds are named.
void PutTextureSet(base::Vector<rx::u8>& b) {
  PutU32(b, 9);
  PutSizedStr(b, "textures\\test\\body.dds");     // 0 diffuse
  PutSizedStr(b, "textures\\test\\body_n.dds");   // 1 normal (alpha = spec mask)
  PutSizedStr(b, "");                            // 2 glow
  PutSizedStr(b, "");                            // 3 height
  PutSizedStr(b, "textures\\cubemaps\\sky.dds");  // 4 environment cubemap
  PutSizedStr(b, "textures\\test\\body_em.dds");  // 5 environment mask
  PutSizedStr(b, "");                            // 6 inner layer / tint mask
  PutSizedStr(b, "");                            // 7 backlight mask
  PutSizedStr(b, "");                            // 8 unused
}

rx::bethesda::NifConversion Convert() {
  base::Vector<rx::u8> node;
  PutAvObject(node);
  PutU32(node, 1);  // child count
  PutI32(node, 1);  // child -> block 1 (the shape)

  base::Vector<rx::u8> shape;
  PutAvObject(shape);
  for (int i = 0; i < 4; ++i)
    PutF32(shape, 0.0f);  // bounding sphere
  PutI32(shape, -1);      // skin
  PutI32(shape, 2);       // shader -> block 2
  PutI32(shape, -1);      // alpha property
  // kHasVertex (desc bit 44), stride nibble 4 (16 bytes): full-precision floats.
  PutU64(shape, (rx::u64(1) << 44) | 0x4);
  const rx::u16 triangles = 1, vertices = 3;
  const rx::u32 stride = 16;
  PutU16(shape, triangles);  // Skyrim: u16 triangle count
  PutU16(shape, vertices);
  PutU32(shape, stride * vertices + 6 * triangles);
  const float pos[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
  for (int v = 0; v < 3; ++v) {
    for (int c = 0; c < 3; ++c)
      PutF32(shape, pos[v][c]);
    PutU32(shape, 0);  // pad to the 16-byte stride
  }
  PutU16(shape, 0);
  PutU16(shape, 1);
  PutU16(shape, 2);

  base::Vector<rx::u8> shader;
  PutShaderProperty(shader, 3);
  base::Vector<rx::u8> textures;
  PutTextureSet(textures);

  base::Vector<rx::u8> b;
  const char* magic = "Gamebryo File Format, Version 20.2.0.7\n";
  for (const char* p = magic; *p; ++p)
    b.push_back(static_cast<rx::u8>(*p));
  PutU32(b, 0x14020007);  // version 20.2.0.7
  PutU8(b, 1);            // little-endian
  PutU32(b, 12);          // user version
  PutU32(b, 4);           // block count
  PutU32(b, 100);         // BS stream version: Skyrim SE
  for (int i = 0; i < 3; ++i)
    PutU8(b, 0);  // 3 empty export strings (bs < 130)
  PutU16(b, 4);   // block type count
  PutSizedStr(b, "NiNode");
  PutSizedStr(b, "BSTriShape");
  PutSizedStr(b, "BSLightingShaderProperty");
  PutSizedStr(b, "BSShaderTextureSet");
  for (rx::u16 i = 0; i < 4; ++i)
    PutU16(b, i);  // block i is of type i
  PutU32(b, static_cast<rx::u32>(node.size()));
  PutU32(b, static_cast<rx::u32>(shape.size()));
  PutU32(b, static_cast<rx::u32>(shader.size()));
  PutU32(b, static_cast<rx::u32>(textures.size()));
  PutU32(b, 0);  // header string count
  PutU32(b, 0);  // max string length
  PutU32(b, 0);  // group count
  b.insert(b.end(), node.begin(), node.end());
  b.insert(b.end(), shape.begin(), shape.end());
  b.insert(b.end(), shader.begin(), shader.end());
  b.insert(b.end(), textures.begin(), textures.end());
  PutU32(b, 1);  // footer: one root
  PutI32(b, 0);

  return rx::bethesda::ConvertNifScene(rx::ByteSpan(b.data(), b.size()),
                                       rx::asset::MakeAssetId("test/shader.nif"),
                                       "test/shader.nif");
}

}  // namespace

int main() {
  std::puts("skyrim lighting shader property:");
  rx::bethesda::NifConversion conv = Convert();
  Check("mesh produced", conv.mesh != nullptr);
  Check("one material", conv.materials.size() == 1);
  if (conv.materials.size() != 1) {
    std::printf("skyrim lighting shader: %d checks FAILED\n", g_failures + 1);
    return 1;
  }
  const rx::asset::Material& m = conv.materials[0];

  // Specular: colour and strength verbatim, masked per texel by the normal
  // map's alpha (the mask flag needs a bound normal map to mean anything).
  CheckNear("specular color r", m.specular_color[0], kSpecularColor[0]);
  CheckNear("specular color g", m.specular_color[1], kSpecularColor[1]);
  CheckNear("specular color b", m.specular_color[2], kSpecularColor[2]);
  CheckNear("specular strength", m.specular_strength, kSpecularStrength);
  Check("normal map bound", static_cast<bool>(m.normal));
  Check("specular masked by the normal alpha", m.specular_mask_in_normal_alpha);

  // Environment mapping reflects, it does not turn the surface into metal.
  CheckNear("env reflect from Environment Map Scale", m.env_reflect, kEnvMapScale);
  CheckNear("not metal", m.metallic_factor, 0.0f);
  Check("env mask bound from slot 5",
        m.env_mask == rx::asset::MakeAssetId("textures/test/body_em.dds"));

  // Lighting effects, each behind its own SLSF2 flag.
  CheckNear("soft lighting from lighting effect 1", m.soft_lighting, kSoftLighting);
  CheckNear("rim lighting from lighting effect 2", m.rim_lighting, kRimPower);
  CheckNear("back lighting off without its flag", m.back_lighting, 0.0f);
  // The fills tint by subsurface_color, which defaults to skin red; anything
  // that is not skin or hair whites it out.
  CheckNear("fill tint neutralized", m.subsurface_color[0], 1.0f);

  // Glossiness still drives roughness (Karis), capped so the reflection reads.
  Check("roughness from glossiness", m.roughness_factor > 0.0f && m.roughness_factor <= 0.35f);

  if (g_failures == 0) {
    std::puts("skyrim lighting shader: all checks passed");
    return 0;
  }
  std::printf("skyrim lighting shader: %d checks FAILED\n", g_failures);
  return 1;
}
