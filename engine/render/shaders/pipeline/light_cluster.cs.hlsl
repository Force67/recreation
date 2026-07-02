#include "rhi_bindings.hlsli"
// Froxel light culling: one thread per cluster, brute-force sphere-vs-AABB
// over the frame's dynamic lights (256 max - cheap enough that smarter
// two-level culling can wait). Each cluster owns a fixed slice of the index
// buffer, so there are no atomics and the forward loop reads a compact list.
struct ClusterPush {
  column_major float4x4 view;  // world -> view
  float2 screen;               // render resolution
  float near_plane;
  float slice_scale;           // slice = log2(viewz) * scale + bias
  float slice_bias;
  uint light_count;
  float tan_half_fov_y;
  float aspect;
};
PUSH_CONSTANTS(ClusterPush, pc);

struct Light {
  float4 pos_radius;
  float4 color_intensity;
  float4 direction_type;
  float4 params;
};
[[vk::binding(0, 0)]] StructuredBuffer<Light> lights : register(t0, space0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> cluster_counts : register(u1, space0);
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> cluster_indices : register(u2, space0);

static const uint kTilesX = 16;
static const uint kTilesY = 9;
static const uint kSlices = 24;
static const uint kMaxPerCluster = 32;

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint cluster = id.x;
  if (cluster >= kTilesX * kTilesY * kSlices) return;
  uint tx = cluster % kTilesX;
  uint ty = (cluster / kTilesX) % kTilesY;
  uint tz = cluster / (kTilesX * kTilesY);

  // Froxel AABB in view space (+x right, +y up, -z forward). Exponential z
  // slices; the xy extent scales with depth through the projection.
  float z0 = exp2((float(tz) - pc.slice_bias) / pc.slice_scale);
  float z1 = exp2((float(tz) + 1.0 - pc.slice_bias) / pc.slice_scale);
  // ndc extents of the tile
  float nx0 = (float(tx) / kTilesX) * 2.0 - 1.0;
  float nx1 = (float(tx + 1u) / kTilesX) * 2.0 - 1.0;
  float ny0 = (float(ty) / kTilesY) * 2.0 - 1.0;
  float ny1 = (float(ty + 1u) / kTilesY) * 2.0 - 1.0;
  float hx = pc.tan_half_fov_y * pc.aspect;
  float hy = pc.tan_half_fov_y;
  float3 mn, mx;
  mn.z = -z1;
  mx.z = -z0;
  mn.x = min(min(nx0 * hx * z0, nx0 * hx * z1), min(nx1 * hx * z0, nx1 * hx * z1));
  mx.x = max(max(nx0 * hx * z0, nx0 * hx * z1), max(nx1 * hx * z0, nx1 * hx * z1));
  // vulkan ndc y-down: ndc -1 is the TOP row; view-space y is up
  mn.y = min(min(-ny0 * hy * z0, -ny0 * hy * z1), min(-ny1 * hy * z0, -ny1 * hy * z1));
  mx.y = max(max(-ny0 * hy * z0, -ny0 * hy * z1), max(-ny1 * hy * z0, -ny1 * hy * z1));

  uint count = 0;
  for (uint li = 0; li < pc.light_count && count < kMaxPerCluster; ++li) {
    Light l = lights[li];
    float3 c = mul(pc.view, float4(l.pos_radius.xyz, 1.0)).xyz;
    float r = l.pos_radius.w;
    float3 closest = clamp(c, mn, mx);
    float3 d = c - closest;
    if (dot(d, d) > r * r) continue;
    cluster_indices[cluster * kMaxPerCluster + count] = li;
    ++count;
  }
  cluster_counts[cluster] = count;
}
