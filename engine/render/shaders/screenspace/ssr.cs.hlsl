#include "rhi_bindings.hlsli"
// Screen-space reflections: a world-space ray march over the prepass depth and
// the lit scene color. The reflection ray reflects the eye vector about the
// surface normal and steps until it crosses behind a depth sample; the hit
// color blends in by a fresnel + screen-edge weight. This is the non-rt
// reflection fallback for low/mobile tiers (rt tiers trace the tlas instead).
// There is no roughness g-buffer, so it leans on fresnel: grazing angles
// reflect, head-on barely does, which reads as the classic wet-floor look.
[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> out_color : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float> depth_map : register(t1, space0);
[[vk::binding(2, 0)]] Texture2D<float2> normal_map : register(t2, space0);
[[vk::binding(3, 0)]] Texture2D<float4> scene_color : register(t3, space0);

struct PushData {
  column_major float4x4 view_proj;      // world -> clip (unjittered)
  column_major float4x4 inv_view_proj;  // clip -> world (unjittered)
  float4 camera_pos;                    // xyz eye, w unused
  float2 inv_size;
  float intensity;       // reflection strength multiplier
  float max_distance;    // world-space march range, meters
  float thickness;       // world-space depth-crossing tolerance, meters
  float frame_index;
  uint step_count;
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

float3 WorldFromUv(float2 uv, float depth) {
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
  if (depth <= 0.0) {  // sky: nothing reflects
    out_color[id.xy] = float4(base, 1.0);
    return;
  }

  float2 uv = (float2(id.xy) + 0.5) * push.inv_size;
  float3 pos = WorldFromUv(uv, depth);
  float3 n = OctDecode(normal_map.Load(p).rg);
  float3 v = normalize(pos - push.camera_pos.xyz);  // eye -> surface
  float3 r = reflect(v, n);                         // reflection direction

  // March in world space from just above the surface; jitter the start so the
  // fixed step does not band (TAA resolves the residual noise).
  float jitter = Ign(float2(id.xy), push.frame_index);
  float step = push.max_distance / float(push.step_count);
  float3 start = pos + n * 0.02;
  float3 hit_color = float3(0, 0, 0);
  float hit_weight = 0.0;
  [loop]
  for (uint i = 0; i < push.step_count; ++i) {
    float t = (float(i) + jitter + 1.0) * step;
    float3 wp = start + r * t;
    float4 clip = mul(push.view_proj, float4(wp, 1.0));
    if (clip.w <= 0.0) break;
    float3 ndc = clip.xyz / clip.w;
    if (abs(ndc.x) > 1.0 || abs(ndc.y) > 1.0) break;  // left the screen
    int2 hp = int2((ndc.xy * 0.5 + 0.5) * size);
    float sdepth = depth_map.Load(int3(hp, 0));
    if (sdepth <= 0.0) continue;  // sky neighbour, no occluder
    float3 surf = WorldFromUv((float2(hp) + 0.5) * push.inv_size, sdepth);
    float ray_d = length(wp - push.camera_pos.xyz);
    float surf_d = length(surf - push.camera_pos.xyz);
    if (ray_d > surf_d && ray_d - surf_d < push.thickness + step) {
      hit_color = scene_color.Load(int3(hp, 0)).rgb;
      float2 edge = smoothstep(0.0, 0.2, 1.0 - abs(ndc.xy));  // fade at borders
      hit_weight = min(edge.x, edge.y);
      break;
    }
  }

  // Schlick fresnel, dielectric f0. v points into the surface so 1 + dot(v, n)
  // is (1 - cos theta).
  float f0 = 0.04;
  float fres = f0 + (1.0 - f0) * pow(saturate(1.0 + dot(v, n)), 5.0);
  float weight = saturate(hit_weight * fres * push.intensity);
  out_color[id.xy] = float4(lerp(base, hit_color, weight), 1.0);
}
