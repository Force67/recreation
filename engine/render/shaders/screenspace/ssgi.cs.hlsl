#include "rhi_bindings.hlsli"
// Screen-space diffuse global illumination: one bounce of indirect light
// gathered from nearby lit surfaces in screen space. Reuses ssao's hemisphere
// disk, but accumulates each tap's lit color weighted by the receiver cosine and
// a back-facing check (the tap must face the receiver to bounce light onto it),
// not just occlusion. This is the non-rt diffuse-gi fallback for tiers without
// ray query; ddgi handles gi when ray tracing is on. There is no albedo
// g-buffer, so it adds a modest bounce fill rather than an albedo-modulated
// term. TAA cleans the gather noise.
[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> out_color : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float> depth_map : register(t1, space0);
[[vk::binding(2, 0)]] Texture2D<float2> normal_map : register(t2, space0);
[[vk::binding(3, 0)]] Texture2D<float4> scene_color : register(t3, space0);

struct PushData {
  column_major float4x4 inv_view_proj;
  float2 inv_size;
  float2 proj_scale;  // {proj.m00, proj.m11}, world radius -> ndc
  float radius;       // gather radius, meters
  float near_plane;
  float intensity;
  float frame_index;
  uint sample_count;
  uint pad;
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

float3 WorldFromTexel(int2 sp, float2 size, float depth) {
  float2 uv = (float2(sp) + 0.5) / size;
  float4 world = mul(push.inv_view_proj, float4(uv * 2.0 - 1.0, depth, 1.0));
  return world.xyz / world.w;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint width, height;
  out_color.GetDimensions(width, height);
  if (id.x >= width || id.y >= height) return;
  float2 size = float2(width, height);
  int3 p = int3(id.xy, 0);

  float3 base = scene_color.Load(p).rgb;
  float depth = depth_map.Load(p);
  if (depth <= 0.0) {  // sky receives no bounce
    out_color[id.xy] = float4(base, 1.0);
    return;
  }

  float3 pos = WorldFromTexel(int2(id.xy), size, depth);
  float3 n = OctDecode(normal_map.Load(p).rg);

  float z_view = push.near_plane / depth;
  float2 radius_uv = push.radius * abs(push.proj_scale) / max(z_view, 1e-3) * 0.5;
  radius_uv = clamp(radius_uv, push.inv_size * 2.0, float2(0.1, 0.1));

  float rot = 6.2831853 * Ign(float2(id.xy), push.frame_index);
  float3 gi = float3(0, 0, 0);
  for (uint i = 0; i < push.sample_count; ++i) {
    float t = (float(i) + 0.5) / float(push.sample_count);
    float ang = rot + float(i) * 2.39996323;  // golden angle disk coverage
    float2 dir2 = float2(cos(ang), sin(ang)) * sqrt(t);
    float2 suv = (float2(id.xy) + 0.5) * push.inv_size + dir2 * radius_uv;

    int2 sp = int2(suv * size);
    if (sp.x < 0 || sp.y < 0 || sp.x >= int(width) || sp.y >= int(height)) continue;
    float sdepth = depth_map.Load(int3(sp, 0));
    if (sdepth <= 0.0) continue;  // sky tap, no surface to bounce from

    float3 spos = WorldFromTexel(sp, size, sdepth);
    float3 d = spos - pos;
    float dist = length(d);
    if (dist < 1e-3) continue;
    float3 dir = d / dist;
    float ndl = saturate(dot(n, dir));               // receiver faces the tap
    float3 sn = OctDecode(normal_map.Load(int3(sp, 0)).rg);
    float facing = saturate(dot(sn, -dir));          // tap emits toward receiver
    float falloff = saturate(1.0 - dist / push.radius);
    gi += scene_color.Load(int3(sp, 0)).rgb * ndl * facing * falloff;
  }
  gi /= float(push.sample_count);

  out_color[id.xy] = float4(base + gi * push.intensity, 1.0);
}
