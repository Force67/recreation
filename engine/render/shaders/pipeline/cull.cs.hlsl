#include "rhi_bindings.hlsli"
// GPU frustum + occlusion culling: one thread per draw instance tests its world
// bounding sphere against the camera frustum and, when enabled, against last
// frame's hi-z (a coarse farthest-depth pyramid), writing the instanceCount
// (1 or 0) into every VkDrawIndexedIndirectCommand the instance owns. The opaque
// prepass and scene then issue those as indirect draws, so off-screen and fully
// occluded geometry costs no raster work. Disabled instances (skinned, no
// bounds) always pass.
struct Instance {
  column_major float4x4 model;
  float4 bounds;  // model-space sphere: xyz center, w radius
  uint first_cmd;
  uint cmd_count;
  uint cull_disabled;
  uint pad;
};
[[vk::binding(0, 0)]] StructuredBuffer<Instance> instances : register(t0, space0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> commands : register(u1, space0);  // 5 u32 per draw command
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> visible_count : register(u2, space0);  // [0]
[[vk::binding(3, 0)]] Texture2D<float> hiz : register(t3, space0);  // last frame, farthest depth

struct PushData {
  float4 planes[5];  // left, right, bottom, top, near (normalized, inside >= 0)
  column_major float4x4 prev_view_proj;
  float4 eye_pad;   // xyz camera eye
  float4 proj_hiz;  // proj.m00, proj.m11, hiz width, hiz height
  uint4 misc;       // instance_count, frustum_enabled, occlusion_enabled, pad
};
PUSH_CONSTANTS(PushData, push);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint i = id.x;
  if (i >= push.misc.x) return;
  Instance inst = instances[i];

  float3 center = mul(inst.model, float4(inst.bounds.xyz, 1.0)).xyz;
  float sx = length(inst.model[0].xyz);
  float sy = length(inst.model[1].xyz);
  float sz = length(inst.model[2].xyz);
  float radius = inst.bounds.w * max(sx, max(sz, sy));
  bool testable = inst.cull_disabled == 0u && inst.bounds.w > 0.0;

  uint visible = 1;
  if (push.misc.y != 0u && testable) {
    [unroll]
    for (int p = 0; p < 5; ++p) {
      if (dot(push.planes[p].xyz, center) + push.planes[p].w < -radius) {
        visible = 0;
        break;
      }
    }
  }

  // Occlusion: project against last frame's view and test the sphere's nearest
  // point against the farthest occluder over its screen footprint. Only small
  // footprints are tested (the coarse hi-z cannot bound large objects safely).
  if (push.misc.z != 0u && visible != 0u && testable) {
    float4 cc = mul(push.prev_view_proj, float4(center, 1.0));
    if (cc.w > 1e-4) {
      float3 ndc = cc.xyz / cc.w;
      float dist = max(length(center - push.eye_pad.xyz), 1e-3);
      float sr = radius * max(push.proj_hiz.x, push.proj_hiz.y) / dist;  // ndc radius
      float3 near_pt = center + normalize(push.eye_pad.xyz - center) * radius;
      float4 nc = mul(push.prev_view_proj, float4(near_pt, 1.0));
      float nearest_z = nc.z / max(nc.w, 1e-4);  // reversed z: larger = closer

      float2 hiz_size = push.proj_hiz.zw;
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
        if (nearest_z < farthest) visible = 0;  // behind even the farthest occluder
      }
    }
  }

  for (uint k = 0; k < inst.cmd_count; ++k) {
    commands[(inst.first_cmd + k) * 5u + 1u] = visible;  // instanceCount field
  }
  if (visible != 0u) InterlockedAdd(visible_count[0], inst.cmd_count);
}
