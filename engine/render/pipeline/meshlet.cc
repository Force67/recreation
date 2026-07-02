#include "render/pipeline/meshlet.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include "core/log.h"
#include "render/rhi/device.h"
#include "shaders/meshlet_ms_hlsl.h"
#include "shaders/meshlet_ps_hlsl.h"

namespace rec::render {
namespace {

constexpr u32 kMaxVerts = 64;
constexpr u32 kMaxTris = 124;  // cone-bounded growth usually finalizes a cluster first

struct MeshletPush {
  Mat4 view_proj;
  f32 planes[5][4];
  f32 camera[4];
};

u32 Part1By2(u32 x) {
  x &= 0x3ff;
  x = (x | (x << 16)) & 0x30000ff;
  x = (x | (x << 8)) & 0x300f00f;
  x = (x | (x << 4)) & 0x30c30c3;
  x = (x | (x << 2)) & 0x9249249;
  return x;
}
u32 Morton3(u32 x, u32 y, u32 z) { return Part1By2(x) | (Part1By2(y) << 1) | (Part1By2(z) << 2); }

// Greedy clustering: append triangles to the current meshlet until it would
// exceed the vertex or triangle budget, then start a new one. Per meshlet a
// bounding sphere and a backface normal cone are computed for cluster culling.
MeshletGeometry BuildImpl(const asset::Vertex* verts, u32 vertex_count, const u32* indices,
                          u32 index_count) {
  MeshletGeometry out;
  auto P = [&](u32 gi) {
    return Vec3{verts[gi].position[0], verts[gi].position[1], verts[gi].position[2]};
  };

  u32 local_global[kMaxVerts];  // local index -> global vertex index
  u8 local_count = 0;
  Vec3 cone_sum{0, 0, 0};  // running sum of unit triangle normals, to bound the cone
  u32 cur_vert_offset = 0, cur_tri_offset = 0;

  auto finalize = [&]() {
    if (local_count == 0) return;
    Meshlet m{};
    m.vertex_offset = cur_vert_offset;
    m.triangle_offset = cur_tri_offset;
    m.vertex_count = local_count;
    m.triangle_count = static_cast<u32>(out.triangles.size()) - cur_tri_offset;

    // Bounding sphere from the meshlet's unique vertices.
    Vec3 center{0, 0, 0};
    for (u32 i = 0; i < local_count; ++i) center = center + P(local_global[i]);
    center = center * (1.0f / static_cast<f32>(local_count));
    f32 radius = 0.0f;
    for (u32 i = 0; i < local_count; ++i) {
      Vec3 d = P(local_global[i]) - center;
      radius = std::max(radius, std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z));
    }
    m.center_radius[0] = center.x;
    m.center_radius[1] = center.y;
    m.center_radius[2] = center.z;
    m.center_radius[3] = radius;

    // Normal cone: average triangle normal as the axis, the widest deviation as
    // the cutoff. A cone wider than a hemisphere can never be wholly backfacing.
    Vec3 axis{0, 0, 0};
    for (u32 t = m.triangle_offset; t < m.triangle_offset + m.triangle_count; ++t) {
      u32 packed = out.triangles[t];
      Vec3 n = Cross(P(local_global[(packed >> 8) & 0xff]) - P(local_global[packed & 0xff]),
                     P(local_global[(packed >> 16) & 0xff]) - P(local_global[packed & 0xff]));
      f32 len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
      if (len > 1e-8f) axis = axis + n * (1.0f / len);
    }
    f32 alen = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    if (alen > 1e-6f) {
      axis = axis * (1.0f / alen);
      f32 min_c = 1.0f;
      for (u32 t = m.triangle_offset; t < m.triangle_offset + m.triangle_count; ++t) {
        u32 packed = out.triangles[t];
        Vec3 n = Cross(P(local_global[(packed >> 8) & 0xff]) - P(local_global[packed & 0xff]),
                       P(local_global[(packed >> 16) & 0xff]) - P(local_global[packed & 0xff]));
        f32 len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        if (len > 1e-8f) min_c = std::min(min_c, (n.x * axis.x + n.y * axis.y + n.z * axis.z) / len);
      }
      m.cone[0] = axis.x;
      m.cone[1] = axis.y;
      m.cone[2] = axis.z;
      m.cone[3] = min_c > 0.0f ? std::sqrt(1.0f - min_c * min_c) : 2.0f;  // 2 = never cull
    } else {
      m.cone[3] = 2.0f;  // degenerate, never cone-cull
    }

    out.meshlets.push_back(m);
    cur_vert_offset = static_cast<u32>(out.vertex_indices.size());
    cur_tri_offset = static_cast<u32>(out.triangles.size());
    local_count = 0;
    cone_sum = {0, 0, 0};
  };

  // Order triangles along a Morton curve over their centroids so the greedy
  // clusters become compact spatial patches (tight normal cones -> effective
  // backface cone culling), not index-order bands that wrap the whole surface.
  const size_t tri_total = index_count / 3;
  Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
  for (u32 i = 0; i < vertex_count; ++i) {
    Vec3 p = P(i);
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  Vec3 ext{std::max(hi.x - lo.x, 1e-6f), std::max(hi.y - lo.y, 1e-6f), std::max(hi.z - lo.z, 1e-6f)};
  std::vector<std::pair<u32, u32>> order(tri_total);  // (morton, triangle index)
  for (size_t t = 0; t < tri_total; ++t) {
    Vec3 c = (P(indices[t * 3]) + P(indices[t * 3 + 1]) + P(indices[t * 3 + 2])) * (1.0f / 3.0f);
    u32 qx = static_cast<u32>(std::clamp((c.x - lo.x) / ext.x, 0.0f, 1.0f) * 1023.0f);
    u32 qy = static_cast<u32>(std::clamp((c.y - lo.y) / ext.y, 0.0f, 1.0f) * 1023.0f);
    u32 qz = static_cast<u32>(std::clamp((c.z - lo.z) / ext.z, 0.0f, 1.0f) * 1023.0f);
    order[t] = {Morton3(qx, qy, qz), static_cast<u32>(t)};
  }
  std::sort(order.begin(), order.end());

  for (const auto& ord : order) {
    size_t i = static_cast<size_t>(ord.second) * 3;
    u32 g[3] = {indices[i], indices[i + 1], indices[i + 2]};
    u8 local[3];
    // Count how many of this triangle's vertices are not already in the meshlet.
    u32 added = 0;
    for (u32 k = 0; k < 3; ++k) {
      bool found = false;
      for (u32 l = 0; l < local_count; ++l) {
        if (local_global[l] == g[k]) {
          found = true;
          break;
        }
      }
      if (!found) ++added;
    }
    // This triangle's unit normal, to keep the meshlet's normal cone tight.
    Vec3 pa = P(g[0]);
    Vec3 n = Cross(P(g[1]) - pa, P(g[2]) - pa);
    f32 nlen = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (nlen > 1e-8f) n = n * (1.0f / nlen);
    // Bound the cone half-angle (~45deg from the running mean) so backface cone
    // culling stays effective; finalize early when a triangle would widen it.
    bool cone_break = false;
    if (local_count > 0) {
      f32 slen = std::sqrt(cone_sum.x * cone_sum.x + cone_sum.y * cone_sum.y + cone_sum.z * cone_sum.z);
      if (slen > 1e-6f && (n.x * cone_sum.x + n.y * cone_sum.y + n.z * cone_sum.z) / slen < 0.85f) {
        cone_break = true;
      }
    }
    bool tri_full = out.triangles.size() - cur_tri_offset >= kMaxTris;
    if (local_count + added > kMaxVerts || tri_full || cone_break) finalize();
    if (nlen > 1e-8f) cone_sum = cone_sum + n;

    for (u32 k = 0; k < 3; ++k) {
      u8 idx = 0xff;
      for (u32 l = 0; l < local_count; ++l) {
        if (local_global[l] == g[k]) {
          idx = static_cast<u8>(l);
          break;
        }
      }
      if (idx == 0xff) {
        idx = local_count;
        local_global[local_count++] = g[k];
        out.vertex_indices.push_back(g[k]);
      }
      local[k] = idx;
    }
    out.triangles.push_back(local[0] | (local[1] << 8) | (local[2] << 16));
  }
  finalize();
  return out;
}

ByteSpan Span(const void* data, size_t bytes) {
  return ByteSpan(static_cast<const u8*>(data), bytes);
}

}  // namespace

MeshletGeometry BuildMeshletGeometry(const asset::Vertex* vertices, u32 vertex_count,
                                     const u32* indices, u32 index_count) {
  return BuildImpl(vertices, vertex_count, indices, index_count);
}

bool MeshletPass::Initialize(Device& device, Format color_format, Format depth_format) {
  available_ = device.caps().mesh_shaders;
  if (!available_) return true;  // no mesh-shader support: pass stays inert, demo skips it

  pipeline_ = device.CreateGraphicsPipeline({
      .fragment = REC_SHADER(k_meshlet_ps_hlsl),
      .mesh = REC_SHADER(k_meshlet_ms_hlsl),
      .raster = {.cull = CullMode::kBack},
      .depth = {.test = true, .write = true, .compare = CompareOp::kGreaterEqual,  // reversed z
                .format = depth_format},
      .color_formats = {color_format},
      .sets = {{.slots = {{0, BindingType::kStorageBuffer},
                          {1, BindingType::kStorageBuffer},
                          {2, BindingType::kStorageBuffer},
                          {3, BindingType::kStorageBuffer},
                          {4, BindingType::kStorageBuffer}},
                .stages = kShaderStageMesh}},
      .push_constant_size = sizeof(MeshletPush),
      .debug_name = "meshlet",
  });
  if (!pipeline_) {
    REC_ERROR("meshlet pipeline creation failed");
    return false;
  }

  for (u32 i = 0; i < kFramesInFlight; ++i) {
    counters_[i] = device.CreateBuffer(16, kBufferUsageStorage, true);
  }
  return true;
}

void MeshletPass::Upload(Device& device, const asset::Mesh& mesh) {
  if (!available_ || mesh.lods.empty()) return;
  device.DestroyBuffer(meshlets_);
  device.DestroyBuffer(meshlet_vertices_);
  device.DestroyBuffer(meshlet_triangles_);
  device.DestroyBuffer(vertices_);

  const asset::MeshLod& lod = mesh.lods[0];
  MeshletGeometry built = BuildMeshletGeometry(lod.vertices.data(),
                                               static_cast<u32>(lod.vertices.size()),
                                               lod.indices.data(),
                                               static_cast<u32>(lod.indices.size()));
  meshlet_count_ = static_cast<u32>(built.meshlets.size());
  if (meshlet_count_ == 0) return;

  // The demo shader pulls a compact position+normal vertex (24 bytes).
  std::vector<Vertex> verts;
  verts.reserve(lod.vertices.size());
  for (const asset::Vertex& v : lod.vertices) {
    verts.push_back({v.position[0], v.position[1], v.position[2], v.normal[0], v.normal[1],
                     v.normal[2]});
  }

  const BufferUsageFlags storage = kBufferUsageStorage;
  meshlets_ = device.CreateBufferWithData(
      Span(built.meshlets.data(), built.meshlets.size() * sizeof(Meshlet)), storage);
  meshlet_vertices_ = device.CreateBufferWithData(
      Span(built.vertex_indices.data(), built.vertex_indices.size() * sizeof(u32)), storage);
  meshlet_triangles_ = device.CreateBufferWithData(
      Span(built.triangles.data(), built.triangles.size() * sizeof(u32)), storage);
  vertices_ = device.CreateBufferWithData(Span(verts.data(), verts.size() * sizeof(Vertex)), storage);
  REC_INFO("meshlet: {} meshlets from {} tris ({} verts)", meshlet_count_,
           mesh.lods[0].indices.size() / 3, mesh.lods[0].vertices.size());
}

u32 MeshletPass::last_visible(u32 slot) const {
  return counters_[slot % kFramesInFlight].mapped
             ? *static_cast<const u32*>(counters_[slot % kFramesInFlight].mapped)
             : 0;
}

void MeshletPass::AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                             const Mat4& view_proj, const f32 planes[5][4], const Vec3& camera,
                             u32 slot) {
  if (!active()) return;
  slot %= kFramesInFlight;
  if (counters_[slot].mapped) *static_cast<u32*>(counters_[slot].mapped) = 0;

  MeshletPush push{};
  push.view_proj = view_proj;
  std::memcpy(push.planes, planes, sizeof(push.planes));
  push.camera[0] = camera.x;
  push.camera[1] = camera.y;
  push.camera[2] = camera.z;
  GpuBuffer counter = counters_[slot];

  graph.AddPass(
      "meshlet",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Write(color, ResourceUsage::kColorAttachment);
        builder.Write(depth, ResourceUsage::kDepthAttachment);
      },
      [this, color, depth, push, counter](PassContext& ctx) {
        const GpuImage& target = ctx.graph->image(color);
        ColorAttachment color_att{.view = target.view, .load = LoadOp::kLoad};
        DepthAttachment depth_att{.view = ctx.graph->image(depth).view, .load = LoadOp::kLoad};
        ctx.cmd->BeginRendering(
            {.extent = target.extent, .colors = {&color_att, 1}, .depth = &depth_att});
        ctx.cmd->BindPipeline(pipeline_);
        ctx.cmd->BindTransient(0, {Bind::StorageBuffer(0, meshlets_),
                                   Bind::StorageBuffer(1, meshlet_vertices_),
                                   Bind::StorageBuffer(2, meshlet_triangles_),
                                   Bind::StorageBuffer(3, vertices_),
                                   Bind::StorageBuffer(4, counter)});
        ctx.cmd->Push(push);
        ctx.cmd->DrawMeshTasks(meshlet_count_, 1, 1);
        ctx.cmd->EndRendering();
      });
}

void MeshletPass::Destroy(Device& device) {
  if (pipeline_) device.DestroyPipeline(pipeline_);
  device.DestroyBuffer(meshlets_);
  device.DestroyBuffer(meshlet_vertices_);
  device.DestroyBuffer(meshlet_triangles_);
  device.DestroyBuffer(vertices_);
  for (u32 i = 0; i < kFramesInFlight; ++i) device.DestroyBuffer(counters_[i]);
}

}  // namespace rec::render
