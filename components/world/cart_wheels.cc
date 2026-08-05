#include "components/world/cart_wheels.h"

#include <base/algorithm.h>
#include <base/containers/unordered_map.h>
#include <base/memory/move.h>
#include <base/strings/to_string.h>

#include <cmath>

namespace rx::world {
namespace {

// A wheel against the whole cart: no smaller than this fraction of the cart's
// longest side (a hub cap is not a wheel) and no larger than this (the cart bed
// is not a wheel either).
constexpr f32 kMinSizeFraction = 0.15f;
constexpr f32 kMaxSizeFraction = 0.80f;
// Thin across the axle, and round about it.
constexpr f32 kMaxThickness = 0.45f;  // thin extent / round extent
constexpr f32 kMaxOvality = 0.30f;    // difference between the two round extents
// Vertices this close together are the same point. Material seams duplicate
// them, and a wheel cut across two materials has to weld back into one island.
constexpr f32 kWeldFraction = 1e-4f;  // of the cart's longest side

struct Bounds {
  Vec3 lo{1e30f, 1e30f, 1e30f};
  Vec3 hi{-1e30f, -1e30f, -1e30f};

  void Add(const f32 p[3]) {
    lo.x = p[0] < lo.x ? p[0] : lo.x;
    lo.y = p[1] < lo.y ? p[1] : lo.y;
    lo.z = p[2] < lo.z ? p[2] : lo.z;
    hi.x = p[0] > hi.x ? p[0] : hi.x;
    hi.y = p[1] > hi.y ? p[1] : hi.y;
    hi.z = p[2] > hi.z ? p[2] : hi.z;
  }
  Vec3 extent() const { return {hi.x - lo.x, hi.y - lo.y, hi.z - lo.z}; }
  Vec3 centre() const { return {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f}; }
  bool empty() const { return hi.x < lo.x; }
};

f32 Axis(const Vec3& v, u32 axis) {
  return axis == 0 ? v.x : axis == 1 ? v.y : v.z;
}

// Union-find over vertex indices.
struct Islands {
  base::Vector<u32> parent;

  explicit Islands(size_t count) {
    parent.reserve(count);
    for (u32 i = 0; i < count; ++i)
      parent.push_back(i);
  }
  u32 Find(u32 i) {
    while (parent[i] != i) {
      parent[i] = parent[parent[i]];
      i = parent[i];
    }
    return i;
  }
  void Join(u32 a, u32 b) {
    a = Find(a);
    b = Find(b);
    if (a != b)
      parent[b] = a;
  }
};

// Copies the triangles `keep` accepts into a fresh mesh, preserving the source's
// per-material submeshes and shifting every vertex by -origin.
asset::Mesh Extract(const asset::Mesh& source,
                    asset::AssetId id,
                    const Vec3& origin,
                    const base::Vector<bool>& keep) {
  asset::Mesh out;
  out.id = id;
  out.exclude_from_rt = source.exclude_from_rt;
  const asset::MeshLod& lod = source.lods[0];
  asset::MeshLod built;
  base::UnorderedMap<u32, u32> remap;  // source vertex -> built vertex
  auto emit = [&](u32 index) {
    if (const u32* found = remap.find(index)) {
      built.indices.push_back(*found);
      return;
    }
    asset::Vertex v = lod.vertices[index];
    v.position[0] -= origin.x;
    v.position[1] -= origin.y;
    v.position[2] -= origin.z;
    const u32 at = static_cast<u32>(built.vertices.size());
    built.vertices.push_back(v);
    remap.insert(index, at);
    built.indices.push_back(at);
  };

  for (const asset::Submesh& submesh : lod.submeshes) {
    const u32 first = static_cast<u32>(built.indices.size());
    for (u32 i = 0; i + 2 < submesh.index_count; i += 3) {
      const u32 triangle = (submesh.index_offset + i) / 3;
      if (triangle >= keep.size() || !keep[triangle])
        continue;
      emit(lod.indices[submesh.index_offset + i + 0]);
      emit(lod.indices[submesh.index_offset + i + 1]);
      emit(lod.indices[submesh.index_offset + i + 2]);
    }
    const u32 count = static_cast<u32>(built.indices.size()) - first;
    if (count)
      built.submeshes.push_back({first, count, submesh.material});
  }
  if (!built.vertices.empty()) {
    Bounds bounds;
    for (const asset::Vertex& v : built.vertices)
      bounds.Add(v.position);
    const Vec3 centre = bounds.centre();
    const Vec3 extent = bounds.extent();
    out.bounds_center[0] = centre.x;
    out.bounds_center[1] = centre.y;
    out.bounds_center[2] = centre.z;
    out.bounds_radius = 0.5f * std::sqrt(extent.x * extent.x + extent.y * extent.y +
                                         extent.z * extent.z);
    out.lods.push_back(base::move(built));
  }
  return out;
}

}  // namespace

CartParts SplitCartWheels(const asset::Mesh& source, const base::String& name, u32 spin_axis) {
  CartParts parts;
  const asset::AssetId id = asset::MakeAssetId(name.c_str());
  parts.body = source;
  parts.body.id = id;
  if (source.lods.empty() || source.lods[0].indices.size() < 3 || spin_axis > 2)
    return parts;
  const asset::MeshLod& lod = source.lods[0];

  Bounds whole;
  for (const asset::Vertex& v : lod.vertices)
    whole.Add(v.position);
  const Vec3 size = whole.extent();
  const f32 longest = base::Max(size.x, base::Max(size.y, size.z));
  if (longest <= 0)
    return parts;

  // Weld coincident vertices, then join every triangle's corners, so an island
  // is a connected piece of the model however its materials cut it up.
  Islands islands(lod.vertices.size());
  {
    const f32 weld = longest * kWeldFraction;
    const f32 inverse = weld > 0 ? 1.0f / weld : 0;
    base::UnorderedMap<u64, u32> grid;
    for (u32 i = 0; i < lod.vertices.size(); ++i) {
      const f32* p = lod.vertices[i].position;
      const i64 x = static_cast<i64>(std::lround((p[0] - whole.lo.x) * inverse));
      const i64 y = static_cast<i64>(std::lround((p[1] - whole.lo.y) * inverse));
      const i64 z = static_cast<i64>(std::lround((p[2] - whole.lo.z) * inverse));
      const u64 key = static_cast<u64>(x & 0x1fffff) | static_cast<u64>(y & 0x1fffff) << 21 |
                      static_cast<u64>(z & 0x1fffff) << 42;
      if (const u32* first = grid.find(key))
        islands.Join(*first, i);
      else
        grid.insert(key, i);
    }
  }
  for (size_t i = 0; i + 2 < lod.indices.size(); i += 3) {
    islands.Join(lod.indices[i], lod.indices[i + 1]);
    islands.Join(lod.indices[i], lod.indices[i + 2]);
  }

  // Measure each island.
  base::UnorderedMap<u32, Bounds> island_bounds;
  for (u32 i = 0; i < lod.vertices.size(); ++i) {
    const u32 root = islands.Find(i);
    if (Bounds* known = island_bounds.find(root)) {
      known->Add(lod.vertices[i].position);
    } else {
      Bounds fresh;
      fresh.Add(lod.vertices[i].position);
      island_bounds.insert(root, fresh);
    }
  }

  // Which islands look like wheels.
  const u32 round_a = (spin_axis + 1) % 3;
  const u32 round_b = (spin_axis + 2) % 3;
  base::Vector<u32> wheel_roots;
  for (auto entry : island_bounds) {
    const Vec3 extent = entry.value.extent();
    const f32 thin = Axis(extent, spin_axis);
    const f32 a = Axis(extent, round_a);
    const f32 b = Axis(extent, round_b);
    const f32 round = base::Max(a, b);
    if (round < longest * kMinSizeFraction || round > longest * kMaxSizeFraction)
      continue;
    if (thin > round * kMaxThickness)
      continue;
    if (std::fabs(a - b) > round * kMaxOvality)
      continue;
    wheel_roots.push_back(entry.key);
  }
  if (wheel_roots.size() < 2)
    return parts;  // nothing wheel-shaped; the caller keeps the mesh as it is

  // Triangle ownership: a triangle belongs to whichever island its corners are
  // in, which after the joins above is the same island for all three.
  const size_t triangles = lod.indices.size() / 3;
  base::Vector<u32> triangle_root(triangles, 0);
  for (size_t t = 0; t < triangles; ++t)
    triangle_root[t] = islands.Find(lod.indices[t * 3]);

  // Order the wheels left-front first, so index 0..3 line up with the front-left,
  // front-right, rear-left, rear-right a four-wheel vehicle expects.
  base::Sort(wheel_roots.begin(), wheel_roots.end(), [&](u32 a, u32 b) {
    const Vec3 ca = island_bounds.find(a)->centre();
    const Vec3 cb = island_bounds.find(b)->centre();
    const f32 fa = Axis(ca, round_b), fb = Axis(cb, round_b);
    if (fa != fb)
      return fa > fb;  // front (further along the forward axis) first
    return Axis(ca, spin_axis) < Axis(cb, spin_axis);
  });

  base::Vector<bool> is_wheel(triangles, false);
  u32 index = 0;
  for (u32 root : wheel_roots) {
    base::Vector<bool> keep(triangles, false);
    for (size_t t = 0; t < triangles; ++t) {
      if (triangle_root[t] != root)
        continue;
      keep[t] = true;
      is_wheel[t] = true;
    }
    const Bounds& bounds = *island_bounds.find(root);
    const Vec3 extent = bounds.extent();
    CartWheel wheel;
    wheel.hub = bounds.centre();
    wheel.radius = 0.5f * base::Max(Axis(extent, round_a), Axis(extent, round_b));
    const base::String wheel_name = name + "/wheel" + base::ToString(static_cast<int>(index));
    wheel.mesh = Extract(source, asset::MakeAssetId(wheel_name.c_str()), wheel.hub, keep);
    ++index;
    if (!wheel.mesh.lods.empty())
      parts.wheels.push_back(base::move(wheel));
  }

  base::Vector<bool> body_keep(triangles, false);
  for (size_t t = 0; t < triangles; ++t)
    body_keep[t] = !is_wheel[t];
  parts.body = Extract(source, id, Vec3{}, body_keep);
  return parts;
}

}  // namespace rx::world
