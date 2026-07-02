#include "rhi_bindings.hlsli"
// Cloud shadows: attenuate the denoised sun shadow by the cloud layer's
// optical depth along the sun ray. The density function is a copy of
// clouds.cs (same fbm, same wind scroll, same coverage mapping) so the shadow
// on the ground tracks the cloud that casts it. Two density taps through the
// slab are plenty at this softness.
struct CloudShadowPush {
  column_major float4x4 inv_view_proj;  // unjittered
  float3 sun_dir;  // travel direction
  float near_plane;
  uint2 size;
  float time;
  float coverage;
  float bottom;    // slab altitudes, metres
  float top;
  float wind;
  float strength;  // max darkening (0..1)
};
PUSH_CONSTANTS(CloudShadowPush, pc);

[[vk::image_format("r8")]] [[vk::binding(0, 0)]] RWTexture2D<float> sun_shadow : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float> depth_map : register(t1, space0);

static const float kGroundRadius = 6360000.0;

// --- density, kept in lockstep with clouds.cs ------------------------------
float Hash3(float3 p) {
  p = frac(p * 0.3183099 + 0.1);
  p *= 17.0;
  return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float Noise3(float3 x) {
  float3 i = floor(x);
  float3 f = frac(x);
  f = f * f * (3.0 - 2.0 * f);
  return lerp(lerp(lerp(Hash3(i), Hash3(i + float3(1, 0, 0)), f.x),
                   lerp(Hash3(i + float3(0, 1, 0)), Hash3(i + float3(1, 1, 0)), f.x), f.y),
              lerp(lerp(Hash3(i + float3(0, 0, 1)), Hash3(i + float3(1, 0, 1)), f.x),
                   lerp(Hash3(i + float3(0, 1, 1)), Hash3(i + float3(1, 1, 1)), f.x), f.y), f.z);
}
float Fbm3(float3 p, int octaves) {
  float v = 0.0;
  float amp = 0.5;
  for (int i = 0; i < octaves; ++i) {
    v += amp * Noise3(p);
    p = p * 2.02 + 33.3;
    amp *= 0.5;
  }
  return v;
}

float CloudDensityAt(float3 p, float h01) {
  float grad = saturate(h01 * 6.0) * saturate((1.0 - h01) * 2.5);
  float3 wind = float3(pc.time * pc.wind, 0.0, pc.time * pc.wind * 0.3);
  float3 wp = (p + wind) * 0.00035;
  float warp = Fbm3(wp * 0.4 + 13.7, 2);
  wp += (warp - 0.5) * 0.9;
  float base = Fbm3(wp, 4);
  float d = saturate(base - (1.0 - pc.coverage)) * grad;
  return d;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= pc.size.x || id.y >= pc.size.y) return;
  float depth = depth_map.Load(int3(id.xy, 0));
  if (depth <= 0.0) return;  // sky shades itself through the cloud pass
  float existing = sun_shadow[id.xy];
  if (existing <= 0.01) return;

  float2 uv = (float2(id.xy) + 0.5) / float2(pc.size);
  float2 ndc = uv * 2.0 - 1.0;
  float4 wh = mul(pc.inv_view_proj, float4(ndc, depth, 1.0));
  float3 world = wh.xyz / wh.w;
  float3 to_sun = normalize(-pc.sun_dir);
  if (to_sun.y <= 0.02) return;  // sun below the layer: no cast shadow

  // The atmosphere frame puts the ground on a sphere; at cast-shadow ranges a
  // flat slab is indistinguishable, so intersect altitudes directly.
  float t0 = (pc.bottom - world.y) / to_sun.y;
  float t1 = (pc.top - world.y) / to_sun.y;
  float3 p0 = world + to_sun * t0;
  float3 p1 = world + to_sun * t1;
  float d = CloudDensityAt(lerp(p0, p1, 0.3), 0.3) + CloudDensityAt(lerp(p0, p1, 0.7), 0.7);
  float shade = exp(-d * 3.0);
  float factor = lerp(1.0 - pc.strength, 1.0, shade);
  sun_shadow[id.xy] = existing * factor;
}
