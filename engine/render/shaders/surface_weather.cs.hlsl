// Surface weather: how precipitation marks the world, applied to the lit scene.
// Rain wets surfaces (darkens them as water fills pores, and adds a glossy sky
// reflection on up-facing puddles); snow settles white on up-facing surfaces.
// Driven by the weather system, modulated by the surface normal (horizontal
// faces wet/snow most). A screen-space pass over the G-buffer normals + depth.

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> out_image;
[[vk::binding(1, 0)]] Texture2D color_in;
[[vk::binding(2, 0)]] Texture2D<float2> normal_map;  // world-space, octahedral
[[vk::binding(3, 0)]] Texture2D depth_in;
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]] TextureCube sky;
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]] SamplerState sky_sampler;

struct PushData {
  column_major float4x4 inv_view_proj;
  float4 camera_pos;  // xyz eye
  float4 params;      // x wetness 0..1, y snow (0 rain / 1 snow), zw unused
  uint2 size;
  uint2 pad;
};
[[vk::push_constant]] PushData pc;

float3 OctDecode(float2 o) {
  float3 d = float3(o.x, 1.0 - abs(o.x) - abs(o.y), o.y);
  if (d.y < 0.0) {
    float2 sign_xz = float2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
    d.xz = (1.0 - abs(d.zx)) * sign_xz;
  }
  return normalize(d);
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
  } else {
    // Snow: settles white on up-facing surfaces.
    float cover = amount * smoothstep(0.35, 0.85, up);
    result = lerp(result, float3(0.88, 0.91, 0.98), cover);
  }
  out_image[id.xy] = float4(result, 1.0);
}
