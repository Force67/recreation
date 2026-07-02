// Resolves the WBOIT accumulation buffers over the opaque scene colour:
// averageColor = accum.rgb / accum.a, then composite by the coverage
// (1 - transmittance), letting the background show through where revealage stays.
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D accum_tex : register(t0, space0);
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState accum_sampler : register(s0, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D reveal_tex : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState reveal_sampler : register(s1, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture2D scene_tex : register(t2, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState scene_sampler : register(s2, space0);

float4 main(float4 pos : SV_Position, [[vk::location(0)]] float2 uv : TEXCOORD0) : SV_Target {
  int3 p = int3(pos.xy, 0);
  float transmittance = reveal_tex.Load(p).r;  // product of (1 - alpha)
  float4 accum = accum_tex.Load(p);
  float3 bg = scene_tex.Load(p).rgb;
  float3 avg = accum.rgb / max(accum.a, 1e-5);
  return float4(avg * (1.0 - transmittance) + bg * transmittance, 1.0);
}
