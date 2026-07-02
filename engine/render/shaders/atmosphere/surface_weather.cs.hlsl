#include "rhi_bindings.hlsli"
// Surface weather: how precipitation marks the world, applied to the lit scene.
// Rain wets surfaces (darkens them as water fills pores, and adds a glossy sky
// reflection on up-facing puddles); snow settles white on up-facing surfaces.
// Driven by the weather system, modulated by the surface normal (horizontal
// faces wet/snow most). A screen-space pass over the G-buffer normals + depth.

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> out_image : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D color_in : register(t1, space0);
[[vk::binding(2, 0)]] Texture2D<float2> normal_map : register(t2, space0);  // world-space, octahedral
[[vk::binding(3, 0)]] Texture2D depth_in : register(t3, space0);
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]] TextureCube sky : register(t4, space0);
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]] SamplerState sky_sampler : register(s4, space0);

struct PushData {
  column_major float4x4 inv_view_proj;
  float4 camera_pos;  // xyz eye
  float4 params;      // x wetness 0..1, y snow (0 rain / 1 snow), z time s, w unused
  uint2 size;
  uint2 pad;
};
PUSH_CONSTANTS(PushData, pc);

float3 OctDecode(float2 o) {
  float3 d = float3(o.x, 1.0 - abs(o.x) - abs(o.y), o.y);
  if (d.y < 0.0) {
    float2 sign_xz = float2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
    d.xz = (1.0 - abs(d.zx)) * sign_xz;
  }
  return normalize(d);
}

float Hash21(float2 p) {
  p = frac(p * float2(123.34, 456.21));
  p += dot(p, p + 45.32);
  return frac(p.x * p.y);
}

// Expanding-ring raindrop ripples on a horizontal water film, anchored to world
// XZ so they sit on the ground instead of swimming with the camera. A 3x3 grid
// of drop cells, each spawning a ring that grows and fades over its lifetime.
float Ripples(float2 wpos, float time) {
  float2 grid = wpos * 3.0;  // cells per metre -> ripple density
  float2 cell = floor(grid);
  float2 f = frac(grid);
  float acc = 0.0;
  [unroll]
  for (int j = -1; j <= 1; ++j) {
    [unroll]
    for (int i = -1; i <= 1; ++i) {
      float2 o = float2(i, j);
      float2 c = cell + o;
      float h = Hash21(c);
      float2 dpos = o + float2(Hash21(c + 3.1), Hash21(c + 7.7));  // jitter in cell
      float life = frac(time * 0.9 + h);                          // 0..1 ring age
      float d = length(f - dpos);
      float ring = exp(-pow((d - life * 0.7) * 14.0, 2.0));       // thin ring at radius
      acc += ring * (1.0 - life);                                 // fade as it ages
    }
  }
  return saturate(acc);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= pc.size.x || id.y >= pc.size.y) return;
  int3 p = int3(id.xy, 0);
  float3 color = color_in.Load(p).rgb;
  float depth = depth_in.Load(p).r;
  if (depth <= 0.0) {  // sky: nothing to wet
    out_image[id.xy] = float4(color, 1.0);
    return;
  }

  float amount = pc.params.x;
  float snow = pc.params.y;
  float3 n = OctDecode(normal_map.Load(p).rg);
  float up = saturate(n.y);  // horizontal surfaces collect water / snow

  float2 uv = (float2(id.xy) + 0.5) / float2(pc.size);
  float4 wp = mul(pc.inv_view_proj, float4(uv * 2.0 - 1.0, depth, 1.0));
  float3 world = wp.xyz / wp.w;
  float3 view = normalize(world - pc.camera_pos.xyz);

  float3 result = color;
  if (snow < 0.5) {
    // Rain: darken, then add a glossy reflection of the sky (puddle sheen),
    // strongest on flat up-facing surfaces and at grazing angles.
    float wet = amount * (0.30 + 0.70 * up);
    result *= lerp(1.0, 0.62, wet);
    float3 refl = reflect(view, n);
    float3 env = sky.SampleLevel(sky_sampler, refl, 0).rgb;
    float fresnel = pow(saturate(1.0 - dot(-view, n)), 4.0);
    result += env * (0.05 + 0.55 * fresnel) * wet * up;
    // Raindrops dimpling the puddle: ripple rings flash the reflected sky on
    // flat wet surfaces, fading out beyond a few metres where they'd alias.
    float dist = length(world - pc.camera_pos.xyz);
    float rip = Ripples(world.xz, pc.params.z) * up * wet * saturate(1.0 - dist / 25.0);
    result += env * rip * 0.5;
  } else {
    // Snow: settles white on up-facing surfaces.
    float cover = amount * smoothstep(0.35, 0.85, up);
    result = lerp(result, float3(0.88, 0.91, 0.98), cover);
  }
  out_image[id.xy] = float4(result, 1.0);
}
