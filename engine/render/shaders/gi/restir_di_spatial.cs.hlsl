#include "rhi_bindings.hlsli"
// Hybrid-path ReSTIR DI, stage 2: spatial reservoir merge + ONE shadow ray
// (inline ray query, force-opaque like the sun's shadow trace) for the
// winning light, shaded into two demodulated targets the forward pass folds
// back in: diffuse irradiance (mesh.ps multiplies by albedo/pi) and a
// specular term with F left out (mesh.ps multiplies by f0). Discrete lights
// need no reuse Jacobian - the target is re-evaluated at the receiver.
// The merged, visibility-checked reservoir feeds back as next frame's
// temporal history, and the pass snapshots depth+normal for reprojection
// validation (the prepass targets are frame-graph transients).
struct RestirDiSpatialPush {
  column_major float4x4 inv_view_proj;  // unjittered
  float4 camera_pos;
  uint2 size;
  uint frame_index;
  uint light_count;
  uint sample_count;  // spatial neighbor taps
  float radius;       // neighbor disk radius, pixels
  float m_max;        // post-merge age cap
  float pad0;
};
PUSH_CONSTANTS(RestirDiSpatialPush, pc);

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> diffuse_out : register(u0, space0);
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> spec_out : register(u1, space0);
[[vk::binding(2, 0)]] Texture2D<float4> r_in : register(t2, space0);
[[vk::binding(3, 0)]] Texture2D<float> depth_map : register(t3, space0);
[[vk::binding(4, 0)]] Texture2D<float4> normal_map : register(t4, space0);
struct PointLight {
  float4 pos_radius;
  float4 color_intensity;
  float4 direction_type;
  float4 params;
};
[[vk::binding(5, 0)]] StructuredBuffer<PointLight> point_lights : register(t5, space0);
[[vk::binding(6, 0)]] RaytracingAccelerationStructure tlas : register(t6, space0);
// Post-merge reservoir history + G-buffer snapshot for next frame.
[[vk::binding(7, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> r_out : register(u7, space0);
[[vk::binding(8, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> prev_depth_out : register(u8, space0);
[[vk::binding(9, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> prev_normal_out : register(u9, space0);

static const float kPi = 3.14159265;

uint Pcg(inout uint state) {
  state = state * 747796405u + 2891336453u;
  uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}
float Rand(inout uint state) { return (Pcg(state) & 0xffffffu) / 16777216.0; }
float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
bool InBounds(int2 p) { return all(p >= 0) && all(p < int2(pc.size)); }

float3 OctDecode(float2 o) {
  float3 d = float3(o.x, 1.0 - abs(o.x) - abs(o.y), o.y);
  if (d.y < 0.0) {
    float2 sign_xz = float2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
    d.xz = (1.0 - abs(d.zx)) * sign_xz;
  }
  return normalize(d);
}

// Same target as the temporal stage (falloff and spot cone match mesh.ps).
float PHatLight(float3 x, float3 n, PointLight pl, out float falloff, out float ndl) {
  falloff = 0.0;
  ndl = 0.0;
  uint ltype = uint(pl.direction_type.w + 0.5);
  if (ltype > 1u) return 0.0;
  float3 to_l = pl.pos_radius.xyz - x;
  float dist2 = dot(to_l, to_l);
  float lr = pl.pos_radius.w;
  if (dist2 >= lr * lr) return 0.0;
  float dist = sqrt(max(dist2, 1e-8));
  ndl = saturate(dot(n, to_l / dist));
  falloff = saturate(1.0 - dist2 / (lr * lr));
  falloff *= falloff;
  if (ltype == 1u) {
    float cd = dot(-(to_l / dist), normalize(pl.direction_type.xyz));
    float att = saturate((cd - pl.params.y) / max(pl.params.x - pl.params.y, 1e-4));
    falloff *= att * att;
  }
  return Luma(pl.color_intensity.rgb) * pl.color_intensity.w * falloff * ndl;
}

float PHat(float3 x, float3 n, float light_id) {
  if (light_id < -0.5) return 0.0;
  uint id = (uint)round(light_id);
  if (id >= pc.light_count) return 0.0;
  float f, ndl;
  return PHatLight(x, n, point_lights[id], f, ndl);
}

float D_GGX(float ndh, float a) {
  float a2 = a * a;
  float d = ndh * ndh * (a2 - 1.0) + 1.0;
  return a2 / max(kPi * d * d, 1e-6);
}
float V_SmithGGXCorrelated(float ndv, float ndl, float a) {
  float a2 = a * a;
  float l = ndv * sqrt(max(ndl * ndl * (1.0 - a2) + a2, 1e-6));
  float v = ndl * sqrt(max(ndv * ndv * (1.0 - a2) + a2, 1e-6));
  return 0.5 / max(l + v, 1e-6);
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  int2 p = int2(tid.xy);
  if (!InBounds(p)) return;

  float depth = depth_map.Load(int3(p, 0));
  float4 nr = normal_map.Load(int3(p, 0));
  prev_depth_out[p] = depth;
  prev_normal_out[p] = nr;
  if (depth <= 0.0) {  // sky
    diffuse_out[p] = 0.0.xxxx;
    spec_out[p] = 0.0.xxxx;
    r_out[p] = float4(-1.0, 0, 0, 0);
    return;
  }
  float2 uv = (float2(p) + 0.5) / float2(pc.size);
  float4 world = mul(pc.inv_view_proj, float4(uv * 2.0 - 1.0, depth, 1.0));
  float3 x = world.xyz / world.w;
  float3 n = OctDecode(nr.rg);
  float roughness = max(nr.b, 0.045);

  uint rng = (tid.y * pc.size.x + tid.x) * 15013u + pc.frame_index * 22699u + 3u;

  // Merge this pixel's reservoir with a few validated neighbors. Weights use
  // the neighbor's stored W with the target re-evaluated here, the standard
  // discrete-light combine.
  float4 self = r_in.Load(int3(p, 0));
  float sel_id = self.x;
  float sel_phat = PHat(x, n, self.x);
  float w_sum = sel_phat * self.w * min(self.z, pc.m_max);
  float M = min(self.z, pc.m_max);
  for (uint s = 0; s < pc.sample_count; ++s) {
    float a = 2.0 * kPi * Rand(rng);
    float r = sqrt(Rand(rng)) * pc.radius;
    int2 q = p + int2(float2(cos(a), sin(a)) * r);
    if (!InBounds(q) || all(q == p)) continue;
    float qd = depth_map.Load(int3(q, 0));
    if (qd <= 0.0 || abs(qd - depth) > 0.1 * max(depth, 1e-4)) continue;
    float3 qn = OctDecode(normal_map.Load(int3(q, 0)).rg);
    if (dot(n, qn) < 0.9) continue;
    float4 nb = r_in.Load(int3(q, 0));
    float nb_M = min(nb.z, pc.m_max);
    float phat = PHat(x, n, nb.x);
    float w = phat * nb.w * nb_M;
    w_sum += w;
    M += nb_M;
    if (w > 0.0 && Rand(rng) * w_sum <= w) {
      sel_id = nb.x;
      sel_phat = phat;
    }
  }
  float W = (sel_phat > 0.0 && M > 0.0) ? w_sum / (sel_phat * M) : 0.0;

  float3 diffuse = 0.0.xxx;
  float3 spec = 0.0.xxx;
  float vis = 0.0;
  if (sel_id >= -0.5 && W > 0.0) {
    PointLight pl = point_lights[(uint)round(sel_id)];
    float falloff, ndl;
    PHatLight(x, n, pl, falloff, ndl);
    if (falloff * ndl > 0.0) {
      float3 to_l = pl.pos_radius.xyz - x;
      float dist = length(to_l);
      float3 l = to_l / dist;

      RayDesc ray;
      ray.Origin = x + n * 0.02;
      ray.Direction = l;
      ray.TMin = 0.0;
      ray.TMax = max(dist - 0.05, 0.0);
      RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> rq;
      rq.TraceRayInline(tlas, RAY_FLAG_NONE, 0xff, ray);
      rq.Proceed();
      vis = rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;

      float3 li = pl.color_intensity.rgb * pl.color_intensity.w * falloff * ndl * W * vis;
      diffuse = li;

      float3 v = normalize(pc.camera_pos.xyz - x);
      float3 h = normalize(l + v);
      float ndv = max(dot(n, v), 1e-4);
      float ndh = max(dot(n, h), 0.0);
      float ggx_a = roughness * roughness;
      spec = li * D_GGX(ndh, ggx_a) * V_SmithGGXCorrelated(ndv, ndl, ggx_a);
    }
  }
  diffuse_out[p] = float4(diffuse, 1.0);
  spec_out[p] = float4(spec, 1.0);
  // Visibility feedback: an occluded winner keeps its age (the sample is
  // memorized as tried) but stops contributing weight until re-validated.
  r_out[p] = float4(sel_id, w_sum * vis, M, W * vis);
}
