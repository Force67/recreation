// Resolves the WBOIT accumulation buffers over the opaque scene colour:
// averageColor = accum.rgb / accum.a, then composite by the coverage
// (1 - transmittance), letting the background show through where revealage stays.
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D accum_tex;
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState accum_sampler;
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D reveal_tex;
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState reveal_sampler;
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture2D scene_tex;
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState scene_sampler;

float4 main(float4 pos : SV_Position, [[vk::location(0)]] float2 uv : TEXCOORD0) : SV_Target {
  int3 p = int3(pos.xy, 0);
  float transmittance = reveal_tex.Load(p).r;  // product of (1 - alpha)
  float4 accum = accum_tex.Load(p);
  float3 bg = scene_tex.Load(p).rgb;
  float3 avg = accum.rgb / max(accum.a, 1e-5);
  return float4(avg * (1.0 - transmittance) + bg * transmittance, 1.0);
}
