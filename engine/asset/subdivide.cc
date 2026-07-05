#include "asset/subdivide.h"

#include <cmath>

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "core/math.h"

namespace rec::asset {
namespace {

Vec3 Pos(const Vertex& v) { return {v.position[0], v.position[1], v.position[2]}; }

void SetPos(Vertex& v, const Vec3& p) {
  v.position[0] = p.x;
  v.position[1] = p.y;
  v.position[2] = p.z;
}

u32 AverageColor(u32 a, u32 b) {
  u32 out = 0;
  for (int i = 0; i < 4; ++i) {
    u32 ca = (a >> (i * 8)) & 0xff;
    u32 cb = (b >> (i * 8)) & 0xff;
    out |= ((ca + cb) / 2u) << (i * 8);
  }
  return out;
}

u64 EdgeKey(u32 a, u32 b) {
  u32 lo = a < b ? a : b;
  u32 hi = a < b ? b : a;
  return (static_cast<u64>(lo) << 32) | hi;
}

// One undirected edge: how many triangles use it (1 = mesh boundary), and the
// up-to-two opposite (third-triangle) vertices used by the interior Loop rule.
struct EdgeRec {
  u32 a = 0;
  u32 b = 0;
  i32 count = 0;
  i32 opp0 = -1;
  i32 opp1 = -1;
};

void SubdivideOnce(MeshLod& lod) {
  const u32 base_count = static_cast<u32>(lod.vertices.size());
  if (base_count == 0 || lod.indices.size() < 3) return;

  // 1. Collect every undirected edge across all submeshes, so an edge shared by
  // two triangles from different materials still counts as interior (no crack).
  base::UnorderedMap<u64, u32> edge_index;
  base::Vector<EdgeRec> edges;
  edges.reserve(lod.indices.size());
  auto touch_edge = [&](u32 a, u32 b, u32 opposite) {
    u64 key = EdgeKey(a, b);
    if (u32* idx = edge_index.find(key)) {
      EdgeRec& e = edges[*idx];
      ++e.count;
      if (e.count == 2) e.opp1 = static_cast<i32>(opposite);
      return;
    }
    u32 idx = static_cast<u32>(edges.size());
    edge_index.insert(key, idx);
    edges.push_back(EdgeRec{a, b, 1, static_cast<i32>(opposite), -1});
  };
  const size_t tri_count = lod.indices.size() / 3;
  for (size_t t = 0; t < tri_count; ++t) {
    u32 a = lod.indices[t * 3 + 0];
    u32 b = lod.indices[t * 3 + 1];
    u32 c = lod.indices[t * 3 + 2];
    touch_edge(a, b, c);
    touch_edge(b, c, a);
    touch_edge(c, a, b);
  }

  // 2. Boundary vertices touch a boundary edge; one-ring neighbours drive the
  // interior smoothing rule.
  base::Vector<u8> is_boundary(base_count);
  base::Vector<base::Vector<u32>> neighbours(base_count);
  for (const EdgeRec& e : edges) {
    neighbours[e.a].push_back(e.b);
    neighbours[e.b].push_back(e.a);
    if (e.count == 1) {
      is_boundary[e.a] = 1;
      is_boundary[e.b] = 1;
    }
  }

  base::Vector<Vertex> out_verts;
  out_verts.reserve(base_count + edges.size());

  // 3. Reposition the original vertices. Boundary vertices are pinned so shared
  // part borders keep lining up; interior vertices use the Loop vertex rule.
  for (u32 v = 0; v < base_count; ++v) {
    Vertex nv = lod.vertices[v];
    if (!is_boundary[v] && !neighbours[v].empty()) {
      u32 n = static_cast<u32>(neighbours[v].size());
      f32 beta = n == 3 ? 3.0f / 16.0f : 3.0f / (8.0f * static_cast<f32>(n));
      Vec3 sum{0, 0, 0};
      for (u32 nb : neighbours[v]) sum += Pos(lod.vertices[nb]);
      Vec3 p = Pos(lod.vertices[v]) * (1.0f - static_cast<f32>(n) * beta) + sum * beta;
      SetPos(nv, p);
    }
    out_verts.push_back(nv);
  }

  // 4. One new vertex per edge (index base_count + edge index), so triangle
  // splitting can look the midpoints up by the same edge index.
  for (const EdgeRec& e : edges) {
    const Vertex& va = lod.vertices[e.a];
    const Vertex& vb = lod.vertices[e.b];
    Vertex mv = va;
    Vec3 p;
    if (e.count >= 2 && e.opp0 >= 0 && e.opp1 >= 0) {
      Vec3 opp = Pos(lod.vertices[e.opp0]) + Pos(lod.vertices[e.opp1]);
      p = (Pos(va) + Pos(vb)) * 0.375f + opp * 0.125f;
    } else {
      p = (Pos(va) + Pos(vb)) * 0.5f;  // boundary edge: linear midpoint
    }
    SetPos(mv, p);
    mv.uv[0] = (va.uv[0] + vb.uv[0]) * 0.5f;
    mv.uv[1] = (va.uv[1] + vb.uv[1]) * 0.5f;
    mv.color = AverageColor(va.color, vb.color);
    out_verts.push_back(mv);
  }

  // 5. Split each triangle into four, keeping every child in its submesh.
  base::Vector<u32> out_indices;
  out_indices.reserve(lod.indices.size() * 4);
  base::Vector<Submesh> out_submeshes;
  out_submeshes.reserve(lod.submeshes.size());
  auto midpoint = [&](u32 a, u32 b) -> u32 {
    return base_count + *edge_index.find(EdgeKey(a, b));
  };
  for (const Submesh& sm : lod.submeshes) {
    Submesh out_sm;
    out_sm.material = sm.material;
    out_sm.index_offset = static_cast<u32>(out_indices.size());
    u32 first_tri = sm.index_offset / 3;
    u32 last_tri = (sm.index_offset + sm.index_count) / 3;
    for (u32 t = first_tri; t < last_tri; ++t) {
      u32 a = lod.indices[t * 3 + 0];
      u32 b = lod.indices[t * 3 + 1];
      u32 c = lod.indices[t * 3 + 2];
      u32 ab = midpoint(a, b);
      u32 bc = midpoint(b, c);
      u32 ca = midpoint(c, a);
      const u32 tris[4][3] = {{a, ab, ca}, {ab, b, bc}, {ca, bc, c}, {ab, bc, ca}};
      for (auto& tr : tris) {
        out_indices.push_back(tr[0]);
        out_indices.push_back(tr[1]);
        out_indices.push_back(tr[2]);
      }
    }
    out_sm.index_count = static_cast<u32>(out_indices.size()) - out_sm.index_offset;
    out_submeshes.push_back(out_sm);
  }

  lod.vertices = std::move(out_verts);
  lod.indices = std::move(out_indices);
  lod.submeshes = std::move(out_submeshes);
}

}  // namespace

void RecomputeNormalsTangents(MeshLod& lod) {
  const size_t n = lod.vertices.size();
  if (n == 0) return;
  base::Vector<Vec3> normals(n);
  base::Vector<Vec3> tangents(n);
  base::Vector<Vec3> bitangents(n);

  const size_t tri_count = lod.indices.size() / 3;
  for (size_t t = 0; t < tri_count; ++t) {
    u32 i0 = lod.indices[t * 3 + 0];
    u32 i1 = lod.indices[t * 3 + 1];
    u32 i2 = lod.indices[t * 3 + 2];
    if (i0 >= n || i1 >= n || i2 >= n) continue;
    const Vertex& v0 = lod.vertices[i0];
    const Vertex& v1 = lod.vertices[i1];
    const Vertex& v2 = lod.vertices[i2];
    Vec3 p0{v0.position[0], v0.position[1], v0.position[2]};
    Vec3 p1{v1.position[0], v1.position[1], v1.position[2]};
    Vec3 p2{v2.position[0], v2.position[1], v2.position[2]};
    Vec3 e1 = p1 - p0;
    Vec3 e2 = p2 - p0;
    Vec3 fn = Cross(e1, e2);  // area-weighted (un-normalized)
    normals[i0] += fn;
    normals[i1] += fn;
    normals[i2] += fn;

    f32 du1 = v1.uv[0] - v0.uv[0], dv1 = v1.uv[1] - v0.uv[1];
    f32 du2 = v2.uv[0] - v0.uv[0], dv2 = v2.uv[1] - v0.uv[1];
    f32 det = du1 * dv2 - du2 * dv1;
    if (std::fabs(det) > 1e-12f) {
      f32 r = 1.0f / det;
      Vec3 tan = (e1 * dv2 - e2 * dv1) * r;
      Vec3 bit = (e2 * du1 - e1 * du2) * r;
      tangents[i0] += tan;
      tangents[i1] += tan;
      tangents[i2] += tan;
      bitangents[i0] += bit;
      bitangents[i1] += bit;
      bitangents[i2] += bit;
    }
  }

  for (size_t i = 0; i < n; ++i) {
    Vec3 nrm = Normalize(normals[i]);
    if (Dot(nrm, nrm) < 0.5f) nrm = {0, 0, 1};
    Vec3 tan = tangents[i];
    tan = tan - nrm * Dot(nrm, tan);  // Gram-Schmidt
    if (Dot(tan, tan) < 1e-10f) {
      // Degenerate uv: pick any axis orthogonal to the normal.
      Vec3 axis = std::fabs(nrm.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
      tan = Cross(axis, nrm);
    }
    tan = Normalize(tan);
    f32 handed = Dot(Cross(nrm, tan), bitangents[i]) < 0 ? -1.0f : 1.0f;
    Vertex& v = lod.vertices[i];
    v.normal[0] = nrm.x;
    v.normal[1] = nrm.y;
    v.normal[2] = nrm.z;
    v.tangent[0] = tan.x;
    v.tangent[1] = tan.y;
    v.tangent[2] = tan.z;
    v.tangent[3] = handed;
  }
}

void SubdivideLoop(MeshLod& lod, u32 levels) {
  for (u32 i = 0; i < levels; ++i) SubdivideOnce(lod);
  RecomputeNormalsTangents(lod);
}

}  // namespace rec::asset
