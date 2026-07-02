#include "rhi_bindings.hlsli"
// Ray traced ambient occlusion: cosine hemisphere rays through the scene TLAS,
// producing a raw normalized hit distance for NRD's REBLUR_DIFFUSE_OCCLUSION
// denoiser (no temporal/spatial filtering here, NRD owns that).
//
// NRD.hlsli supplies the hit-distance packing. NRD is an optional dependency, so
// when it isn't vendored (e.g. CI, mobile) fall back to a self-contained copy of
// REBLUR_FrontEnd_GetNormHitDist; this pass runs undenoised in that case but must
// still compile.
#if __has_include("NRD.hlsli")
#include "NRD.hlsli"
#else
float REBLUR_FrontEnd_GetNormHitDist(float hit_dist, float view_z, float3 params,
                                     float roughness) {
  float smc = (1.0 - exp2(-200.0 * roughness * roughness)) * pow(saturate(roughness), 0.5);
  float f = (params.x + abs(view_z) * params.y) * lerp(params.z, 1.0, smc);
  return max(saturate(hit_dist / f), 1e-6);
}
#endif

[[vk::image_format("r8")]] [[vk::binding(0, 0)]] RWTexture2D<float> hitdist_out : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float> depth_map : register(t1, space0);
[[vk::binding(2, 0)]] Texture2D<float2> normal_map : register(t2, space0);
[[vk::binding(3, 0)]] RaytracingAccelerationStructure tlas : register(t3, space0);

struct PushData {
  column_major float4x4 inv_view_proj;  // unjittered
  float2 inv_size;
  float radius;
  float near_plane;
  float hit_a;  // REBLUR A, B, C (must match ReblurSettings), kept as scalars
  float hit_b;  // to avoid float3 push-constant alignment surprises
  float hit_c;
  float frame_index;
  uint ray_count;
};
PUSH_CONSTANTS(PushData, push);

float3 OctDecode(float2 o) {
  float3 d = float3(o.x, 1.0 - abs(o.x) - abs(o.y), o.y);
  if (d.y < 0.0) {
    float2 sign_xz = float2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
    d.xz = (1.0 - abs(d.zx)) * sign_xz;
  }
  return normalize(d);
}

float Ign(float2 pixel, float offset) {
  float ign = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
  return frac(ign + offset * 0.61803398875);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint width, height;
  hitdist_out.GetDimensions(width, height);
  if (id.x >= width || id.y >= height) return;
  int3 p = int3(id.xy, 0);

  float depth = depth_map.Load(p);
  if (depth <= 0.0) {  // sky: fully unoccluded
    hitdist_out[id.xy] = 1.0;
    return;
  }

  float2 uv = (float2(id.xy) + 0.5) * push.inv_size;
  float2 ndc = uv * 2.0 - 1.0;
  float4 world = mul(push.inv_view_proj, float4(ndc, depth, 1.0));
  float3 world_pos = world.xyz / world.w;
  float3 n = OctDecode(normal_map.Load(p).rg);

  float3 up = abs(n.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
  float3 t = normalize(cross(up, n));
  float3 b = cross(n, t);

  float hit_sum = 0.0;
  for (uint ray_index = 0; ray_index < push.ray_count; ++ray_index) {
    float u1 = Ign(float2(id.xy), push.frame_index + ray_index * 13.0);
    float u2 = Ign(float2(id.yx) + 31.0, push.frame_index * 1.7 + ray_index * 7.0);
    float cos_theta = sqrt(1.0 - u1);
    float sin_theta = sqrt(u1);
    float phi = 6.2831853 * u2;
    float3 dir = t * (cos(phi) * sin_theta) + b * (sin(phi) * sin_theta) + n * cos_theta;

    RayDesc ray;
    ray.Origin = world_pos + n * 0.02;
    ray.TMin = 0.001;
    ray.Direction = dir;
    ray.TMax = push.radius;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> rq;
    rq.TraceRayInline(tlas, RAY_FLAG_NONE, 0xff, ray);
    rq.Proceed();
    // Misses contribute the full radius (no occluder), so the average tracks
    // how close blockers are.
    hit_sum += rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? rq.CommittedRayT() : push.radius;
  }
  float avg_hit = hit_sum / push.ray_count;

  float viewz = push.near_plane / depth;
  float3 hit_dist_params = float3(push.hit_a, push.hit_b, push.hit_c);
  hitdist_out[id.xy] = REBLUR_FrontEnd_GetNormHitDist(avg_hit, viewz, hit_dist_params, 1.0);
}
