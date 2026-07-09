// nif_starfieldtest: deterministic check that the Starfield ".mesh" decoder
// reads the index/position/uv/color streams of the format. Starfield (BS stream
// 173) moved geometry out of the NIF into hash-keyed ".mesh" files, so the unit
// under test is ParseStarfieldMesh against a hand-built buffer (version, index
// count, u16 indices, dequant scale, weights-per-vertex, vertex count, i16
// positions, then the fixed uv1/uv2/color/normal/tangent stream sequence). No
// game data needed, so it runs in the ctest gate.

#include <cstdio>
#include <cstring>
#include <vector>

#include "bethesda/starfield_mesh.h"
#include "core/types.h"

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

void PutU16(std::vector<rx::u8>& b, rx::u16 v) {
  b.push_back(rx::u8(v));
  b.push_back(rx::u8(v >> 8));
}
void PutU32(std::vector<rx::u8>& b, rx::u32 v) {
  for (int i = 0; i < 4; ++i) b.push_back(rx::u8(v >> (8 * i)));
}
void PutI16(std::vector<rx::u8>& b, rx::i16 v) { PutU16(b, static_cast<rx::u16>(v)); }
void PutF32(std::vector<rx::u8>& b, float f) {
  rx::u32 v;
  std::memcpy(&v, &f, 4);
  PutU32(b, v);
}

}  // namespace

int main() {
  std::puts("starfield mesh decode:");

  // A unit quad: four corners at the extents of the dequant range, two
  // triangles. scale 2.0 metres so a corner decodes to +/-2 metres.
  const float scale = 2.0f;
  const rx::i16 q = 32767;
  const rx::i16 pos[4][3] = {
      {-q, -q, 0}, {q, -q, 0}, {q, q, 0}, {-q, q, 0}};
  const rx::u16 indices[6] = {0, 1, 2, 0, 2, 3};

  std::vector<rx::u8> b;
  PutU32(b, 2);  // version
  PutU32(b, 6);  // index count
  for (rx::u16 i : indices) PutU16(b, i);
  PutF32(b, scale);
  PutU32(b, 0);  // weights per vertex
  PutU32(b, 4);  // vertex count
  for (int v = 0; v < 4; ++v)
    for (int c = 0; c < 3; ++c) PutI16(b, pos[v][c]);
  // uv1 present (two float16 per vertex, zero here), uv2/color/normal/tangent
  // absent so the trailing streams are just zero counts.
  PutU32(b, 4);  // uv1 count
  for (int v = 0; v < 4; ++v) PutU32(b, 0);
  PutU32(b, 0);  // uv2
  PutU32(b, 0);  // color
  PutU32(b, 0);  // normals
  PutU32(b, 0);  // tangents

  rx::bethesda::StarfieldMeshData mesh;
  bool ok = rx::bethesda::ParseStarfieldMesh(rx::ByteSpan(b.data(), b.size()), &mesh);
  Check("mesh parsed", ok);
  Check("four vertices decoded", mesh.vertices.size() == 4);
  Check("six indices decoded", mesh.indices.size() == 6);

  if (mesh.vertices.size() == 4) {
    // Corner 1 sits at +q on x, +scale metres, lifted to game units (~70/m), so
    // ~140 units. Allow a wide tolerance for the quantization round trip.
    float x = mesh.vertices[1].position[0];
    Check("position lifted to game units (~140)", x > 130.0f && x < 150.0f);
    // The quad faces +z, so geometric normals point along z.
    float nz = mesh.vertices[0].normal[2];
    Check("recomputed normal faces z", nz > 0.9f || nz < -0.9f);
  }

  // A truncated buffer (header only) must fail rather than read past the end.
  rx::bethesda::StarfieldMeshData truncated;
  Check("short buffer rejected",
        !rx::bethesda::ParseStarfieldMesh(rx::ByteSpan(b.data(), 8), &truncated));

  if (g_failures == 0) {
    std::puts("starfield mesh: all checks passed");
    return 0;
  }
  std::printf("starfield mesh: %d checks FAILED\n", g_failures);
  return 1;
}
