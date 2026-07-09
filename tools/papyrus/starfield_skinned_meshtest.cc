// starfield_skinned_meshtest: deterministic checks for the Starfield skinned
// ".mesh" parser. It builds a minimal synthetic mesh in memory (version, index
// list, positions, the five optional vertex streams, then the weight stream)
// and confirms the parser recovers indices and positions and reduces each
// vertex's influences to its top four, renormalized to u8 weights summing to
// 255.

#include <cstdio>
#include <cstring>
#include <vector>

#include "bethesda/starfield_mesh.h"
#include "core/types.h"

namespace {

using rx::f32;
using rx::i16;
using rx::u16;
using rx::u32;
using rx::u8;

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

void PutU16(std::vector<u8>& b, u16 v) { b.insert(b.end(), {u8(v), u8(v >> 8)}); }
void PutI16(std::vector<u8>& b, i16 v) { PutU16(b, static_cast<u16>(v)); }
void PutU32(std::vector<u8>& b, u32 v) {
  for (int i = 0; i < 4; ++i) b.push_back(u8(v >> (8 * i)));
}
void PutF32(std::vector<u8>& b, f32 v) {
  u32 bits;
  std::memcpy(&bits, &v, 4);
  PutU32(b, bits);
}

}  // namespace

int main() {
  // Three vertices, one triangle. weightsPerVertex = 6 so the reduction to four
  // is exercised; scale = 1.0 keeps positions easy to read back.
  constexpr u32 kVertexCount = 3;
  constexpr u32 kWpv = 6;
  const i16 positions[kVertexCount][3] = {
      {0, 0, 0},
      {32767, 0, 0},   // maps to +1.0 on x
      {0, -32767, 0},  // maps to -1.0 on y
  };

  std::vector<u8> mesh;
  PutU32(mesh, 2);  // version
  PutU32(mesh, 3);  // indexCount
  PutU16(mesh, 0);
  PutU16(mesh, 1);
  PutU16(mesh, 2);
  PutF32(mesh, 1.0f);  // scale
  PutU32(mesh, kWpv);  // weightsPerVertex
  PutU32(mesh, kVertexCount);
  for (u32 v = 0; v < kVertexCount; ++v)
    for (int k = 0; k < 3; ++k) PutI16(mesh, positions[v][k]);
  // Five optional streams, all absent (count 0) so the weight stream follows.
  for (int s = 0; s < 5; ++s) PutU32(mesh, 0);

  // Weight stream. vertex 0 has six influences whose two smallest must be
  // dropped; the kept four renormalize to sum 255. vertices 1 and 2 pad with
  // zero-weight entries.
  PutU32(mesh, kVertexCount * kWpv);  // influenceCount
  const u16 v0[kWpv][2] = {{5, 100}, {2, 4000}, {7, 2000}, {9, 1000}, {3, 500}, {1, 30000}};
  for (auto& inf : v0) {
    PutU16(mesh, inf[0]);
    PutU16(mesh, inf[1]);
  }
  for (u32 v = 1; v < kVertexCount; ++v) {
    PutU16(mesh, 11);     // bone of the single real influence
    PutU16(mesh, 65535);  // full weight
    for (u32 j = 1; j < kWpv; ++j) {
      PutU16(mesh, 0);
      PutU16(mesh, 0);
    }
  }

  rx::bethesda::StarfieldSkinnedMeshData out;
  bool parsed = rx::bethesda::ParseStarfieldSkinnedMesh(
      rx::ByteSpan(mesh.data(), mesh.size()), &out);

  std::puts("starfield skinned mesh:");
  Check("parses", parsed);
  Check("vertex count", out.vertices.size() == kVertexCount);
  Check("skinning count", out.skinning.size() == kVertexCount);
  Check("index count", out.indices.size() == 3);
  Check("indices recovered", out.indices.size() == 3 && out.indices[0] == 0 &&
                                 out.indices[1] == 1 && out.indices[2] == 2);

  if (out.vertices.size() == kVertexCount) {
    const f32* p1 = out.vertices[1].position;
    const f32* p2 = out.vertices[2].position;
    Check("position v1 (+x metres)", p1[0] > 0.999f && p1[0] < 1.001f);
    Check("position v2 (-y metres)", p2[1] < -0.999f && p2[1] > -1.001f);
  }

  if (out.skinning.size() == kVertexCount) {
    const rx::asset::SkinnedVertexExtra& e0 = out.skinning[0];
    // Top four by weight are bones 1(30000), 2(4000), 7(2000), 9(1000); the two
    // smallest (5=100, 3=500) are dropped.
    bool right_bones = e0.bone_indices[0] == 1 && e0.bone_indices[1] == 2 &&
                       e0.bone_indices[2] == 7 && e0.bone_indices[3] == 9;
    Check("top-four bones, weight-ordered", right_bones);
    u32 sum0 = u32(e0.bone_weights[0]) + e0.bone_weights[1] + e0.bone_weights[2] + e0.bone_weights[3];
    Check("weights sum to 255", sum0 == 255);
    Check("dominant bone has the largest weight",
          e0.bone_weights[0] > e0.bone_weights[1] && e0.bone_weights[1] >= e0.bone_weights[2]);

    const rx::asset::SkinnedVertexExtra& e1 = out.skinning[1];
    Check("single-influence bone index", e1.bone_indices[0] == 11);
    Check("single-influence weight is full", e1.bone_weights[0] == 255);
  }

  if (g_failures == 0) {
    std::puts("starfield_skinned_mesh: all checks passed");
    return 0;
  }
  std::printf("starfield_skinned_mesh: %d checks FAILED\n", g_failures);
  return 1;
}
