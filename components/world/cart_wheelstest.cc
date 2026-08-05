// cart_wheelstest: SplitCartWheels finds the wheels in a cart mesh by shape
// alone. Builds a synthetic wagon (a slab body plus four thin discs on two
// axles, cut across two materials the way a real export is) and checks that the
// discs come out hub-centred with the body left behind, and that a mesh with no
// wheels in it is handed back untouched.

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include <cmath>
#include <cstdio>

#include "components/world/cart_wheels.h"
#include "core/types.h"

using namespace rx;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    ++g_failures;
}

void AddTriangle(asset::MeshLod* lod, const Vec3& a, const Vec3& b, const Vec3& c) {
  for (const Vec3& p : {a, b, c}) {
    asset::Vertex v{};
    v.position[0] = p.x;
    v.position[1] = p.y;
    v.position[2] = p.z;
    v.normal[1] = 1;
    lod->indices.push_back(static_cast<rx::u32>(lod->vertices.size()));
    lod->vertices.push_back(v);
  }
}

// A disc in the y/z plane (thin across x), as a fan of `segments` triangles.
// `half` splits it across two calls so the wheel arrives in two materials, which
// is how a real export cuts it and what the weld has to put back together.
void AddDisc(asset::MeshLod* lod,
             const Vec3& hub,
             f32 radius,
             rx::u32 from,
             rx::u32 to,
             rx::u32 segments) {
  for (rx::u32 i = from; i < to; ++i) {
    const f32 a0 = 6.2831853f * static_cast<f32>(i) / static_cast<f32>(segments);
    const f32 a1 = 6.2831853f * static_cast<f32>(i + 1) / static_cast<f32>(segments);
    const Vec3 p0{hub.x, hub.y + std::sin(a0) * radius, hub.z + std::cos(a0) * radius};
    const Vec3 p1{hub.x, hub.y + std::sin(a1) * radius, hub.z + std::cos(a1) * radius};
    AddTriangle(lod, hub, p0, p1);
  }
}

void AddBox(asset::MeshLod* lod, const Vec3& centre, const Vec3& half) {
  // Two triangles per face is enough: only the bounds matter to the classifier.
  const Vec3 lo{centre.x - half.x, centre.y - half.y, centre.z - half.z};
  const Vec3 hi{centre.x + half.x, centre.y + half.y, centre.z + half.z};
  AddTriangle(lod, lo, Vec3{hi.x, lo.y, lo.z}, hi);
  AddTriangle(lod, lo, hi, Vec3{lo.x, hi.y, hi.z});
}

void CloseSubmesh(asset::MeshLod* lod, rx::u32 from, rx::u64 material) {
  lod->submeshes.push_back(
      {from, static_cast<rx::u32>(lod->indices.size()) - from, asset::AssetId{material}});
}

}  // namespace

int main() {
  constexpr rx::u32 kSegments = 24;
  constexpr f32 kRadius = 0.6f;
  const Vec3 hubs[4] = {{-0.9f, 0.6f, 1.4f}, {0.9f, 0.6f, 1.4f}, {-0.9f, 0.6f, -1.4f},
                        {0.9f, 0.6f, -1.4f}};

  asset::Mesh cart;
  cart.lods.push_back({});
  asset::MeshLod& lod = cart.lods[0];
  // Material 1: the body and the front half of every wheel.
  rx::u32 from = 0;
  AddBox(&lod, Vec3{0, 1.2f, 0}, Vec3{0.85f, 0.5f, 2.0f});
  for (const Vec3& hub : hubs)
    AddDisc(&lod, hub, kRadius, 0, kSegments / 2, kSegments);
  CloseSubmesh(&lod, from, 1);
  // Material 2: the rest of every wheel.
  from = static_cast<rx::u32>(lod.indices.size());
  for (const Vec3& hub : hubs)
    AddDisc(&lod, hub, kRadius, kSegments / 2, kSegments, kSegments);
  CloseSubmesh(&lod, from, 2);

  std::puts("splitting a cart:");
  world::CartParts parts = world::SplitCartWheels(cart, "test/cart");
  Check("four wheels found", parts.wheels.size() == 4);
  Check("the body is still there", !parts.body.lods.empty());

  bool hubs_ok = parts.wheels.size() == 4;
  bool centred = hubs_ok;
  bool sized = hubs_ok;
  for (const world::CartWheel& wheel : parts.wheels) {
    bool matched = false;
    for (const Vec3& hub : hubs)
      if (Length(wheel.hub - hub) < 0.05f)
        matched = true;
    hubs_ok = hubs_ok && matched;
    sized = sized && std::fabs(wheel.radius - kRadius) < 0.05f;
    // Hub-centred: every vertex of the wheel is within its own radius of zero.
    for (const asset::Vertex& v : wheel.mesh.lods[0].vertices)
      centred = centred && std::fabs(v.position[0]) < 0.05f &&
                std::sqrt(v.position[1] * v.position[1] + v.position[2] * v.position[2]) <
                    kRadius + 0.05f;
  }
  Check("each wheel sits on an axle hub", hubs_ok);
  Check("each wheel measures its own radius", sized);
  Check("each wheel is recentred on its hub", centred);
  Check("both materials survive the split",
        !parts.wheels.empty() && parts.wheels[0].mesh.lods[0].submeshes.size() == 2);

  {
    // The body keeps only the slab: four discs of kSegments triangles each came
    // out of it.
    const size_t body_triangles = parts.body.lods.empty()
                                      ? 0
                                      : parts.body.lods[0].indices.size() / 3;
    Check("the wheels are gone from the body", body_triangles == 2);
  }

  std::puts("a cart with no wheels:");
  asset::Mesh slab;
  slab.lods.push_back({});
  AddBox(&slab.lods[0], Vec3{0, 1.2f, 0}, Vec3{0.85f, 0.5f, 2.0f});
  CloseSubmesh(&slab.lods[0], 0, 1);
  world::CartParts none = world::SplitCartWheels(slab, "test/slab");
  Check("nothing is split out", !none.split());
  Check("the mesh comes back whole",
        !none.body.lods.empty() && none.body.lods[0].indices.size() == slab.lods[0].indices.size());

  std::printf("%s\n", g_failures ? "FAILED" : "all ok");
  return g_failures ? 1 : 0;
}
