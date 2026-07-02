#include "rhi_bindings.hlsli"
// Task (amplification) stage for the optional mesh-shader opaque path. One
// workgroup per 32 meshlets of one (lod, submesh): frustum-cull the whole
// instance once, then frustum- and backface-cone-cull each meshlet and compact
// the survivors into a payload, so the mesh shader only launches workgroups for
// the clusters that actually contribute. Geometry arrives by device address.

struct FrameGlobals {
  column_major float4x4 view_proj;
  column_major float4x4 prev_view_proj;
  column_major float4x4 inv_view_proj;
  float2 jitter;
  float2 prev_jitter;
  float4 sun_direction;
  float4 sun_color;
  float4 camera_position;  // xyz eye
  float4 misc;
  uint flags;
  float3 pad;
};
[[vk::binding(0, 0)]] ConstantBuffer<FrameGlobals> frame : register(b0, space0);
[[vk::binding(2, 0)]] Texture2D<float> hiz : register(t2, space0);  // last frame's coarse farthest-depth pyramid

struct PushData {
  column_major float4x4 model;
  column_major float4x4 prev_model;
  float4 bounds;      // xyz model-space instance center, w radius (instance cull)
  float4 occlusion;   // proj.m00, proj.m11, hiz width, hiz height (hiz w == 0 disables)
  uint64_t meshlets_addr;
  uint64_t meshlet_vertices_addr;
  uint64_t meshlet_triangles_addr;
  uint64_t vertices_addr;
  uint meshlet_offset;
  uint meshlet_count;
};
PUSH_CONSTANTS(PushData, push);

struct MeshPayload {
  uint meshlet[32];
};

static const uint kMeshletStride = 48;
groupshared uint s_count;
groupshared MeshPayload s_payload;

float LoadF(uint64_t base, uint off) { return vk::RawBufferLoad<float>(base + off); }

void FrustumPlanes(float4x4 m, out float4 pl[5]) {
  pl[0] = m[3] + m[0];
  pl[1] = m[3] - m[0];
  pl[2] = m[3] + m[1];
  pl[3] = m[3] - m[1];
  pl[4] = m[2];  // reversed-z near
}

bool OutsideFrustum(float4 pl[5], float3 c, float r) {
  [unroll]
  for (int p = 0; p < 5; ++p) {
    float3 n = pl[p].xyz;
    float inv = rsqrt(max(dot(n, n), 1e-12));
    if ((dot(n, c) + pl[p].w) * inv < -r) return true;
  }
  return false;
}

// Hi-z occlusion of the whole instance against last frame's farthest-depth
// pyramid: project the world bounds, compare the nearest point against the
// farthest occluder over its (small) screen footprint. Ports cull.cs. Only
// small footprints are tested (the coarse hi-z cannot bound large objects).
bool InstanceOccluded(float3 mcenter, float mradius) {
  if (push.occlusion.z < 1.0) return false;  // hi-z unavailable / disabled
  float3 center = mul(push.model, float4(mcenter, 1.0)).xyz;
  float sx = length(push.model[0].xyz);
  float sy = length(push.model[1].xyz);
  float sz = length(push.model[2].xyz);
  float radius = mradius * max(sx, max(sy, sz));
  float4 cc = mul(frame.prev_view_proj, float4(center, 1.0));
  if (cc.w <= 1e-4) return false;
  float3 ndc = cc.xyz / cc.w;
  float dist = max(length(center - frame.camera_position.xyz), 1e-3);
  float sr = radius * max(push.occlusion.x, push.occlusion.y) / dist;  // ndc radius
  float3 near_pt = center + normalize(frame.camera_position.xyz - center) * radius;
  float4 nc = mul(frame.prev_view_proj, float4(near_pt, 1.0));
  float nearest_z = nc.z / max(nc.w, 1e-4);  // reversed z: larger = closer
  float2 hiz_size = push.occlusion.zw;
  int2 t_lo = int2(((ndc.xy - sr) * 0.5 + 0.5) * hiz_size);
  int2 t_hi = int2(((ndc.xy + sr) * 0.5 + 0.5) * hiz_size);
  const int kMaxTexels = 8;
  if (t_lo.x >= 0 && t_lo.y >= 0 && t_hi.x < int(hiz_size.x) && t_hi.y < int(hiz_size.y) &&
      (t_hi.x - t_lo.x) <= kMaxTexels && (t_hi.y - t_lo.y) <= kMaxTexels) {
    float farthest = 1.0;  // reversed z: smallest value over the footprint
    for (int y = t_lo.y; y <= t_hi.y; ++y) {
      for (int x = t_lo.x; x <= t_hi.x; ++x) {
        farthest = min(farthest, hiz.Load(int3(x, y, 0)));
      }
    }
    if (nearest_z < farthest) return true;  // behind even the farthest occluder
  }
  return false;
}

[numthreads(32, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID, uint gtid : SV_GroupIndex) {
  if (gtid == 0) s_count = 0;
  GroupMemoryBarrierWithGroupSync();

  // Model-space frustum planes from view_proj*model; instance cull is uniform.
  float4x4 mvp = mul(frame.view_proj, push.model);
  float4 planes[5];
  FrustumPlanes(mvp, planes);
  bool instance_out = OutsideFrustum(planes, push.bounds.xyz, push.bounds.w) ||
                      InstanceOccluded(push.bounds.xyz, push.bounds.w);

  uint local = dtid.x;
  bool emit = false;
  uint meshlet_index = 0;
  if (!instance_out && local < push.meshlet_count) {
    meshlet_index = push.meshlet_offset + local;
    uint64_t mb = push.meshlets_addr + (uint64_t)meshlet_index * kMeshletStride;
    float3 center = float3(LoadF(mb, 0), LoadF(mb, 4), LoadF(mb, 8));
    float radius = LoadF(mb, 12);
    float3 cone = float3(LoadF(mb, 16), LoadF(mb, 20), LoadF(mb, 24));
    float cutoff = LoadF(mb, 28);
    bool culled = OutsideFrustum(planes, center, radius);
    float3 world_center = mul(push.model, float4(center, 1.0)).xyz;
    float3 world_axis = normalize(mul((float3x3)push.model, cone));
    float3 view_dir = normalize(world_center - frame.camera_position.xyz);
    if (dot(view_dir, world_axis) >= cutoff) culled = true;
    emit = !culled;
  }

  if (emit) {
    uint slot;
    InterlockedAdd(s_count, 1, slot);
    s_payload.meshlet[slot] = meshlet_index;
  }
  GroupMemoryBarrierWithGroupSync();
  DispatchMesh(s_count, 1, 1, s_payload);
}
