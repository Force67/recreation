// tritest: deterministic checks for the Skyrim .tri (FRTRI003) morph parser. It
// builds a tiny synthetic tri in memory (3 vertices, 1 face, UVs, 2 named
// morphs) and confirms the parser recovers the vertex count, morph names,
// scales and i16 deltas, that FindMorph and ApplyMorph behave, and that bad
// input is rejected.

#include <cstdio>
#include <cstring>
#include <vector>

#include "bethesda/tri.h"
#include "core/types.h"

namespace {

using rx::f32;
using rx::i16;
using rx::u32;
using rx::u8;

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

void PutU32(std::vector<u8>& b, u32 v) {
  for (int i = 0; i < 4; ++i) b.push_back(u8(v >> (8 * i)));
}
void PutI16(std::vector<u8>& b, i16 v) {
  auto u = static_cast<rx::u16>(v);
  b.insert(b.end(), {u8(u), u8(u >> 8)});
}
void PutF32(std::vector<u8>& b, f32 v) {
  u32 bits;
  std::memcpy(&bits, &v, 4);
  PutU32(b, bits);
}
void PutName(std::vector<u8>& b, const char* s) {
  u32 len = static_cast<u32>(std::strlen(s)) + 1;  // includes null terminator
  PutU32(b, len);
  for (const char* p = s; *p; ++p) b.push_back(static_cast<u8>(*p));
  b.push_back(0);
}

std::vector<u8> BuildTri() {
  constexpr u32 kV = 3, kF = 1;
  std::vector<u8> b;
  for (char c : {'F', 'R', 'T', 'R', 'I', '0', '0', '3'}) b.push_back(static_cast<u8>(c));
  PutU32(b, kV);   // vertex_count
  PutU32(b, kF);   // face_count
  PutU32(b, 0);    // unknown_0c
  PutU32(b, 0);    // unknown_10
  PutU32(b, 0);    // unknown_14
  PutU32(b, kV);   // uv_count
  PutU32(b, 1);    // flags: has UVs
  PutU32(b, 2);    // morph_count
  PutU32(b, 0);    // modifier_count
  PutU32(b, 0);    // modifier_vertex_count
  for (int i = 0; i < 4; ++i) PutU32(b, 0);  // unknown_30..3c

  for (u32 v = 0; v < kV * 3; ++v) PutF32(b, static_cast<f32>(v));  // base vertices
  for (u32 i = 0; i < kF * 3; ++i) PutU32(b, i);                    // vertex indices
  for (u32 i = 0; i < kV * 2; ++i) PutF32(b, 0.25f);               // uvs
  for (u32 i = 0; i < kF * 3; ++i) PutU32(b, i);                    // uv indices

  // morph 0: "TestA", scale 0.5, deltas 1,2,3, 4,5,6, 7,8,9
  PutName(b, "TestA");
  PutF32(b, 0.5f);
  for (i16 d = 1; d <= 9; ++d) PutI16(b, d);
  // morph 1: "B", scale 2.0, deltas all -10
  PutName(b, "B");
  PutF32(b, 2.0f);
  for (int i = 0; i < 9; ++i) PutI16(b, -10);
  return b;
}

}  // namespace

int main() {
  using namespace rx::bethesda;
  std::puts("tri parser:");

  std::vector<u8> buf = BuildTri();
  auto set = ParseTri(rx::ByteSpan(buf.data(), buf.size()));
  Check("parses a valid FRTRI003 blob", set.has_value());
  if (!set) {
    std::printf("tri: %d checks FAILED\n", ++g_failures);
    return 1;
  }

  Check("vertex count", set->vertex_count == 3);
  Check("two morphs, no modifiers", set->morphs.size() == 2 && set->modifiers.empty());
  Check("morph 0 name", set->morphs[0].name == "TestA");
  Check("morph 0 scale", set->morphs[0].scale == 0.5f);
  Check("morph 0 delta count matches vertices", set->morphs[0].deltas.size() == 3);
  Check("morph 0 delta[1]", set->morphs[0].deltas[1].x == 4 && set->morphs[0].deltas[1].y == 5 &&
                                set->morphs[0].deltas[1].z == 6);
  Check("morph 1 name and scale", set->morphs[1].name == "B" && set->morphs[1].scale == 2.0f);

  const TriMorph* found = set->FindMorph("B");
  Check("FindMorph hits", found != nullptr && found->name == "B");
  Check("FindMorph misses on unknown", set->FindMorph("Nope") == nullptr);

  // ApplyMorph: base + weight * scale * delta. Vertex 0 delta = (1,2,3),
  // scale 0.5, weight 2 -> +(1,2,3).
  f32 pos[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
  ApplyMorph(set->morphs[0], 2.0f, pos, 3);
  Check("ApplyMorph vertex 0", pos[0] == 1.0f && pos[1] == 2.0f && pos[2] == 3.0f);
  Check("ApplyMorph vertex 2", pos[6] == 7.0f && pos[7] == 8.0f && pos[8] == 9.0f);

  // Rejection cases.
  Check("rejects bad magic", !ParseTri(rx::ByteSpan(reinterpret_cast<const u8*>("NOTTRI00"), 8)));
  Check("rejects truncation", !ParseTri(rx::ByteSpan(buf.data(), buf.size() - 4)));

  if (g_failures == 0) {
    std::puts("tri: all checks passed");
    return 0;
  }
  std::printf("tri: %d checks FAILED\n", g_failures);
  return 1;
}
