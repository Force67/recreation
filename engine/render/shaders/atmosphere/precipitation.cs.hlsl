#include "rhi_bindings.hlsli"
// Screen-space precipitation: layered procedural rain streaks or snow flakes,
// composited over the lit scene. Anchored in world (azimuth, altitude) of the
// view ray so it scrolls correctly with the camera instead of swimming. Driven
// by the weather system's precipitation amount + snow flag. Cheap; no particle
// simulation.

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> out_image : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D color_in : register(t1, space0);

struct PushData {
  column_major float4x4 inv_view_proj;
  float4 camera_pos;  // xyz eye, w time (s)
  float4 params;      // x intensity 0..1, y snow (0 rain / 1 snow), zw unused
  uint2 size;
  uint2 pad;
};
PUSH_CONSTANTS(PushData, pc);

float Hash21(float2 p) {
  p = frac(p * float2(123.34, 345.45));
  p += dot(p, p + 34.345);
  return frac(p.x * p.y);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= pc.size.x || id.y >= pc.size.y) return;
  int2 px = int2(id.xy);
  float3 scene = color_in.Load(int3(px, 0)).rgb;
  float intensity = pc.params.x;
  float snow = pc.params.y;
  float time = pc.camera_pos.w;
  if (intensity <= 0.0) {
    out_image[px] = float4(scene, 1.0);
    return;
  }

  // World-space view ray -> spherical (azimuth, altitude) for a stable anchor.
  float2 ndc = (float2(px) + 0.5) / float2(pc.size) * 2.0 - 1.0;
  float4 nh = mul(pc.inv_view_proj, float4(ndc, 1.0, 1.0));
  float3 dir = normalize(nh.xyz / nh.w - pc.camera_pos.xyz);
  float az = atan2(dir.x, dir.z);
  float alt = asin(clamp(dir.y, -1.0, 1.0));
  // Azimuth degenerates at the zenith/nadir (dir.x, dir.z -> 0), where the
  // streak anchor pinwheels. Fade precipitation out within ~10 deg of straight
  // up/down to hide it; you mostly see sky or ground there anyway.
  float pole = 1.0 - smoothstep(0.85, 0.985, abs(dir.y));

  float3 add = 0.0.xxx;
  const int kLayers = 4;
  if (snow < 0.5) {
    // Rain: parallax layers (near = long, fast, sparse; far = short, dense) of
    // soft motion-blurred streaks with heavy per-streak variation (length,
    // brightness, jitter) so it never reads as a regular grid.
    const float tilt = 0.16;  // wind lean (couples x into y)
    const int kRainLayers = 5;
    float wet = 0.0;
    for (int l = 0; l < kRainLayers; ++l) {
      float fl = float(l) / float(kRainLayers - 1);  // 0 near .. 1 far
      float scale_x = lerp(85.0, 240.0, fl);         // far = denser across
      float scale_y = lerp(14.0, 50.0, fl);          // near = taller cells = longer streaks
      float speed = lerp(24.0, 10.0, fl);            // near = faster
      float2 p;
      p.x = az * scale_x;
      p.y = -alt * scale_y + time * speed + p.x * tilt;
      float2 cell = floor(p);
      float2 f = frac(p);
      float r1 = Hash21(cell + fl * 31.7 + 1.0);  // presence
      float r2 = Hash21(cell + fl * 13.3 + 5.0);  // x jitter
      float r3 = Hash21(cell + fl * 7.7 + 9.0);   // brightness
      float r4 = Hash21(cell + fl * 23.9 + 3.0);  // length
      if (r1 < 0.80) continue;                     // sparse
      float dx = f.x - 0.5 - (r2 - 0.5) * 0.55;    // jittered, soft gaussian width
      float core = exp(-dx * dx * 70.0);
      float len = lerp(0.4, 0.92, r4);             // varied streak length
      float along = smoothstep(0.0, 0.04, f.y) * smoothstep(len, len - 0.3, f.y);
      wet += core * along * (0.35 + 0.65 * r3) * lerp(1.0, 0.55, fl);
    }
    wet = saturate(wet * intensity);
    // Translucent + scene-aware: rain catches light, so it reads bright against
    // dark surfaces and stays subtle against the bright sky, like the real thing.
    float scene_luma = dot(scene, float3(0.299, 0.587, 0.114));
    add = lerp(float3(0.85, 0.92, 1.05), float3(0.32, 0.38, 0.50), saturate(scene_luma)) * wet;
  } else {
    // Snow: slow drifting soft round flakes with a gentle sway.
    float total = 0.0;
    for (int l = 0; l < kLayers; ++l) {
      float fl = float(l);
      float scale = 26.0 + fl * 16.0;
      float speed = 0.8 + fl * 0.6;
      float sway = sin(time * 0.6 + fl * 2.1) * 0.25;
      float2 p;
      p.x = (az + sway) * scale;
      p.y = -alt * scale + time * speed;
      float2 cell = floor(p);
      float2 f = frac(p) - 0.5;
      float h = Hash21(cell + fl * 11.3);
      float present = step(0.93, h);
      total += present * smoothstep(0.28, 0.0, length(f));
    }
    add = float3(1.0, 1.0, 1.0) * total * intensity * 0.85;
  }

  out_image[px] = float4(scene + add * pole, 1.0);
}
