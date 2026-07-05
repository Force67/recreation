// facemorphtest: deterministic checks for the runtime head pipeline that has no
// game-data dependency: Loop subdivision (engine/asset/subdivide) and the
// NAM9/NAMA -> chargen morph mapping (engine/bethesda/head_morph).

#include <cmath>
#include <cstdio>
#include <string>

#include <base/containers/vector.h>

#include "asset/mesh.h"
#include "asset/subdivide.h"
#include "bethesda/head_morph.h"
#include "core/types.h"

namespace {

using rec::f32;
using rec::i32;
using rec::u32;

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

rec::asset::Vertex MakeVert(f32 x, f32 y, f32 z, f32 u, f32 v) {
  rec::asset::Vertex vert{};
  vert.position[0] = x;
  vert.position[1] = y;
  vert.position[2] = z;
  vert.uv[0] = u;
  vert.uv[1] = v;
  vert.color = 0xffffffffu;
  return vert;
}

// A unit quad in the z = 0 plane, two triangles sharing the diagonal.
rec::asset::MeshLod MakeQuad() {
  rec::asset::MeshLod lod;
  lod.vertices.push_back(MakeVert(0, 0, 0, 0, 0));  // 0
  lod.vertices.push_back(MakeVert(1, 0, 0, 1, 0));  // 1
  lod.vertices.push_back(MakeVert(1, 1, 0, 1, 1));  // 2
  lod.vertices.push_back(MakeVert(0, 1, 0, 0, 1));  // 3
  const u32 idx[] = {0, 1, 2, 0, 2, 3};
  for (u32 i : idx) lod.indices.push_back(i);
  lod.submeshes.push_back({0, 6, rec::asset::AssetId{}});
  return lod;
}

void TestNormals() {
  std::printf("normal/tangent recompute:\n");
  rec::asset::MeshLod lod = MakeQuad();
  rec::asset::RecomputeNormalsTangents(lod);
  bool all_facing = true;
  for (const rec::asset::Vertex& v : lod.vertices) {
    // Flat z=0 quad wound CCW: normals should be +Z, unit length.
    if (std::fabs(v.normal[2] - 1.0f) > 1e-3f) all_facing = false;
    f32 len = std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] +
                        v.normal[2] * v.normal[2]);
    if (std::fabs(len - 1.0f) > 1e-3f) all_facing = false;
    f32 tlen = std::sqrt(v.tangent[0] * v.tangent[0] + v.tangent[1] * v.tangent[1] +
                         v.tangent[2] * v.tangent[2]);
    if (std::fabs(tlen - 1.0f) > 1e-3f) all_facing = false;
  }
  Check("flat quad normals are unit +Z, tangents unit length", all_facing);
}

void TestSubdivide() {
  std::printf("loop subdivision (boundary preserving):\n");
  rec::asset::MeshLod lod = MakeQuad();
  // Capture the 4 corner positions before subdividing.
  f32 corners[4][3];
  for (int i = 0; i < 4; ++i)
    for (int k = 0; k < 3; ++k) corners[i][k] = lod.vertices[i].position[k];

  rec::asset::SubdivideLoop(lod, 1);
  // 4 originals + 5 edges (4 boundary + 1 shared diagonal) = 9 vertices;
  // 2 triangles -> 8 triangles = 24 indices.
  Check("vertex count 4 -> 9", lod.vertices.size() == 9);
  Check("triangle count 2 -> 8", lod.indices.size() == 24);

  // Every corner is on the mesh boundary, so it must stay pinned.
  bool corners_fixed = true;
  for (int i = 0; i < 4; ++i)
    for (int k = 0; k < 3; ++k)
      if (std::fabs(lod.vertices[i].position[k] - corners[i][k]) > 1e-5f) corners_fixed = false;
  Check("boundary corners held fixed", corners_fixed);

  // A boundary-edge midpoint (0->1) must be the linear midpoint (0.5, 0, 0).
  bool found_mid = false;
  for (const rec::asset::Vertex& v : lod.vertices) {
    if (std::fabs(v.position[0] - 0.5f) < 1e-5f && std::fabs(v.position[1]) < 1e-5f &&
        std::fabs(v.position[2]) < 1e-5f)
      found_mid = true;
  }
  Check("boundary edge 0-1 splits at its linear midpoint", found_mid);

  // Subdividing again must not crash and must keep growing.
  rec::asset::SubdivideLoop(lod, 1);
  Check("second subdivision grows the mesh", lod.vertices.size() > 9);

  // Level 0 only recomputes normals: geometry unchanged.
  rec::asset::MeshLod flat = MakeQuad();
  rec::asset::SubdivideLoop(flat, 0);
  Check("subdiv level 0 leaves 4 vertices", flat.vertices.size() == 4);
}

void TestMorphMapping() {
  std::printf("NAM9/NAMA -> chargen morph mapping:\n");
  f32 nam9[rec::bethesda::kNam9Count] = {};
  // index 0 = NoseLong (negative -> NoseShort), 1 = NoseUp (positive), 9 = BrowsUp.
  nam9[0] = -1.0f;  // NoseLong slider pushed negative
  nam9[1] = 0.5f;   // NoseUp
  nam9[9] = -0.8f;  // BrowsUp negative -> BrowDown
  i32 nama[4] = {3, -1, 17, 4};  // nose type 3, eyes 17, mouth 4, brows none

  base::Vector<rec::bethesda::MorphWeight> out;
  rec::bethesda::CollectFaceMorphs(nam9, nama, &out);

  auto weight_of = [&](const char* name) -> f32 {
    for (const auto& w : out)
      if (w.name == name) return w.weight;
    return -999.0f;
  };
  auto has = [&](const char* name) { return weight_of(name) != -999.0f; };

  Check("NoseLong -1 -> NoseShort @ 1.0", std::fabs(weight_of("NoseShort") - 1.0f) < 1e-4f);
  Check("no NoseLong emitted for negative value", !has("NoseLong"));
  Check("NoseUp +0.5 -> NoseUp @ 0.5", std::fabs(weight_of("NoseUp") - 0.5f) < 1e-4f);
  Check("BrowsUp -0.8 -> BrowDown @ 0.8", std::fabs(weight_of("BrowDown") - 0.8f) < 1e-4f);
  Check("NAMA nose 3 -> NoseType3 @ 1.0", std::fabs(weight_of("NoseType3") - 1.0f) < 1e-4f);
  Check("NAMA eyes 17 -> EyesType17 @ 1.0", std::fabs(weight_of("EyesType17") - 1.0f) < 1e-4f);
  Check("NAMA mouth 4 -> LipType4 @ 1.0", std::fabs(weight_of("LipType4") - 1.0f) < 1e-4f);
  Check("NAMA brows -1 emits nothing", !has("BrowType-1") && !has("BrowType1"));

  // A single-sided slider (JawForward, index 4) drops a negative value.
  f32 nam9b[rec::bethesda::kNam9Count] = {};
  nam9b[4] = -0.5f;
  base::Vector<rec::bethesda::MorphWeight> out2;
  rec::bethesda::CollectFaceMorphs(nam9b, nama, &out2);
  bool jaw = false;
  for (const auto& w : out2)
    if (w.name == "JawForward") jaw = true;
  Check("single-sided JawForward ignores a negative value", !jaw);

  // Slider labels enumerate for the UI.
  Check("slider 1 label is NoseUp",
        std::string(rec::bethesda::Nam9SliderInfo(1).label) == "NoseUp");
}

}  // namespace

int main() {
  std::printf("facemorphtest\n");
  TestNormals();
  TestSubdivide();
  TestMorphMapping();
  std::printf("%s (%d failures)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
  return g_failures == 0 ? 0 : 1;
}
