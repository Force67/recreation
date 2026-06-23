// nif_fo4test: deterministic check that the NIF reader parses Fallout 4 mesh
// geometry. FO4 (BS stream 130) widens the BSTriShape triangle count to u32,
// where Skyrim (100) uses u16; a hand-built minimal FO4 NIF (one NiNode root
// over one BSTriShape with three vertices and one triangle) exercises that path.
// If the count width were wrong the data-size check would fail and no mesh would
// come back. No game data needed, so it runs in the ctest gate; Skyrim geometry
// is covered by the tests that load shipped meshes.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "asset/asset_id.h"
#include "bethesda/nif.h"
#include "core/types.h"

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

void PutU8(std::vector<rec::u8>& b, rec::u8 v) { b.push_back(v); }
void PutU16(std::vector<rec::u8>& b, rec::u16 v) {
  b.push_back(rec::u8(v));
  b.push_back(rec::u8(v >> 8));
}
void PutU32(std::vector<rec::u8>& b, rec::u32 v) {
  for (int i = 0; i < 4; ++i) b.push_back(rec::u8(v >> (8 * i)));
}
void PutI32(std::vector<rec::u8>& b, rec::i32 v) { PutU32(b, static_cast<rec::u32>(v)); }
void PutU64(std::vector<rec::u8>& b, rec::u64 v) {
  for (int i = 0; i < 8; ++i) b.push_back(rec::u8(v >> (8 * i)));
}
void PutF32(std::vector<rec::u8>& b, float f) {
  rec::u32 v;
  std::memcpy(&v, &f, 4);
  PutU32(b, v);
}
void PutSizedStr(std::vector<rec::u8>& b, const char* s) {  // u32 len + bytes
  rec::u32 n = 0;
  while (s[n]) ++n;
  PutU32(b, n);
  for (rec::u32 i = 0; i < n; ++i) b.push_back(static_cast<rec::u8>(s[i]));
}

// NiAVObject prefix the reader consumes: name, extra list, controller, flags,
// transform (translation, 3x3 rotation, scale), collision ref. 72 bytes here.
void PutAvObject(std::vector<rec::u8>& b) {
  PutI32(b, -1);  // name (no string table entry)
  PutU32(b, 0);   // extra data count
  PutI32(b, -1);  // controller
  PutU32(b, 0);   // flags (bit 0 = hidden; visible here)
  for (int i = 0; i < 3; ++i) PutF32(b, 0.0f);  // translation
  const float ident[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  for (float v : ident) PutF32(b, v);  // rotation
  PutF32(b, 1.0f);  // scale
  PutI32(b, -1);    // collision object
}

}  // namespace

int main() {
  std::puts("fallout 4 nif geometry:");

  // Block 0: the root NiNode with one child (the shape, block 1).
  std::vector<rec::u8> node;
  PutAvObject(node);
  PutU32(node, 1);   // child count
  PutI32(node, 1);   // child -> block 1

  // Block 1: a BSTriShape with three vertices and one triangle.
  std::vector<rec::u8> shape;
  PutAvObject(shape);
  for (int i = 0; i < 4; ++i) PutF32(shape, 0.0f);  // bounding sphere (center+radius)
  PutI32(shape, -1);  // skin
  PutI32(shape, -1);  // shader
  PutI32(shape, -1);  // alpha property
  // Vertex desc: kHasVertex (flag bit 0 -> desc bit 44), stride nibble = 4 (16
  // bytes), no other attributes, so positions are full-precision floats.
  PutU64(shape, (rec::u64(1) << 44) | 0x4);
  const rec::u32 triangles = 1, stride = 16;
  const rec::u16 vertices = 3;
  PutU32(shape, triangles);   // FO4: u32 triangle count
  PutU16(shape, vertices);
  PutU32(shape, stride * vertices + 6 * triangles);  // data size = 54
  const float pos[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
  for (int v = 0; v < 3; ++v) {
    for (int c = 0; c < 3; ++c) PutF32(shape, pos[v][c]);
    PutU32(shape, 0);  // 4 bytes padding to the 16-byte stride
  }
  PutU16(shape, 0);
  PutU16(shape, 1);
  PutU16(shape, 2);  // one triangle

  Check("node block is 80 bytes", node.size() == 80);
  Check("shape block is 172 bytes", shape.size() == 172);

  // Assemble: magic line, header, the two blocks, footer.
  std::vector<rec::u8> b;
  const char* magic = "Gamebryo File Format, Version 20.2.0.7\n";
  for (const char* p = magic; *p; ++p) b.push_back(static_cast<rec::u8>(*p));
  PutU32(b, 0x14020007);  // version 20.2.0.7
  PutU8(b, 1);            // little-endian
  PutU32(b, 12);          // user version
  PutU32(b, 2);           // block count
  PutU32(b, 130);         // BS stream version: Fallout 4
  for (int i = 0; i < 4; ++i) PutU8(b, 0);  // 4 empty export strings (bs >= 130)
  PutU16(b, 2);           // block type count
  PutSizedStr(b, "NiNode");
  PutSizedStr(b, "BSTriShape");
  PutU16(b, 0);  // block 0 type index
  PutU16(b, 1);  // block 1 type index
  PutU32(b, static_cast<rec::u32>(node.size()));
  PutU32(b, static_cast<rec::u32>(shape.size()));
  PutU32(b, 0);  // header string count
  PutU32(b, 0);  // max string length
  PutU32(b, 0);  // group count
  b.insert(b.end(), node.begin(), node.end());
  b.insert(b.end(), shape.begin(), shape.end());
  PutU32(b, 1);   // footer: one root
  PutI32(b, 0);   // root -> block 0 (the node)

  rec::bethesda::NifConversion conv = rec::bethesda::ConvertNifScene(
      rec::ByteSpan(b.data(), b.size()), rec::asset::MakeAssetId("test/synthetic.nif"),
      "test/synthetic.nif");
  Check("mesh produced", conv.mesh != nullptr);
  if (conv.mesh && !conv.mesh->lods.empty()) {
    const auto& lod = conv.mesh->lods[0];
    Check("three vertices decoded", lod.vertices.size() == 3);
    Check("three indices decoded", lod.indices.size() == 3);
    Check("one submesh", lod.submeshes.size() == 1);
  } else {
    Check("mesh has a lod", false);
  }

  if (g_failures == 0) {
    std::puts("fallout 4 nif: all checks passed");
    return 0;
  }
  std::printf("fallout 4 nif: %d checks FAILED\n", g_failures);
  return 1;
}
