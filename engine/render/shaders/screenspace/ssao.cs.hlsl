#include "rhi_bindings.hlsli"
// Screen-space ambient occlusion. Hemisphere kernel over the prepass depth and
// world normals: each tap reconstructs a neighbour's world position and adds
// occlusion by how much it rises above the surface, weighted by N.dot and a
// linear range falloff. No ray tracing, no denoiser; TAA smooths the noise.
[[vk::image_format("r8")]] [[vk::binding(0, 0)]] RWTexture2D<float> ao_out : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float> depth_map : register(t1, space0);
[[vk::binding(2, 0)]] Texture2D<float2> normal_map : register(t2, space0);

struct PushData {
  column_major float4x4 inv_view_proj;  // unjittered
  float2 inv_size;
  float2 proj_scale;  // {proj.m00, proj.m11}, world radius -> ndc
  float radius;       // meters
  float near_plane;
  float intensity;
  float bias;
  float power;
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
  ao_out.GetDimensions(width, height);
  if (id.x >= width || id.y >= height) return;
  float2 size = float2(width, height);
  int3 p = int3(id.xy, 0);

  float depth = depth_map.Load(p);
  if (depth <= 0.0) {  // sky: fully unoccluded
    ao_out[id.xy] = 1.0;
    return;
  }

  float3 pos = WorldFromTexel(int2(id.xy), size, depth);
  float3 n = OctDecode(normal_map.Load(p).rg);

  // World radius projected to a uv-space disk at this depth (reversed z), then
  // clamped to an effective pixel band so distant surfaces still sample real
  // neighbours instead of collapsing onto the same texel.
  float z_view = push.near_plane / depth;
  float2 radius_uv = push.radius * abs(push.proj_scale) / max(z_view, 1e-3) * 0.5;
  radius_uv = clamp(radius_uv, push.inv_size * 2.0, float2(0.06, 0.06));

  float rot = 6.2831853 * Ign(float2(id.xy), push.frame_index);
  float occ = 0.0;
  for (uint i = 0; i < push.sample_count; ++i) {
    float t = (float(i) + 0.5) / float(push.sample_count);
    float ang = rot + float(i) * 2.39996323;  // golden angle, even disk coverage
    float2 dir = float2(cos(ang), sin(ang)) * sqrt(t);
    float2 suv = (float2(id.xy) + 0.5) * push.inv_size + dir * radius_uv;

    int2 sp = int2(suv * size);
    if (sp.x < 0 || sp.y < 0 || sp.x >= int(width) || sp.y >= int(height)) continue;
    float sdepth = depth_map.Load(int3(sp, 0));
    if (sdepth <= 0.0) continue;  // sky neighbour, no occluder

    float3 v = WorldFromTexel(sp, size, sdepth) - pos;
    float d = length(v);
    if (d < 1e-4) continue;
    float falloff = saturate(1.0 - d / push.radius);
    occ += max(0.0, dot(n, v / d) - push.bias) * falloff;
  }

  occ /= float(push.sample_count);
  float ao = pow(saturate(1.0 - push.intensity * occ), push.power);
  ao_out[id.xy] = ao;
}
