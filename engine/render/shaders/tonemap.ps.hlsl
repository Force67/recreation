[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D scene;
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState scene_sampler;

struct PushData {
  float exposure;
  uint tonemap;  // 0 aces, 1 reinhard, 2 none
};
[[vk::push_constant]] PushData push;

// Narkowicz ACES fit. Cheap, no LUT, good enough until a proper grading
// stage with white balance lands.
float3 TonemapAces(float3 x) {
  return clamp(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

// The swapchain is UNORM, the engine owns the transfer function.
float3 SrgbEncode(float3 c) {
  return lerp(c * 12.92, 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055, step(0.0031308, c));
}

float4 main(float4 sv_position : SV_Position,
            [[vk::location(0)]] float2 uv : TEXCOORD0) : SV_Target0 {
  float3 hdr = scene.Sample(scene_sampler, uv).rgb * push.exposure;
  float3 ldr;
  if (push.tonemap == 0u) {
    ldr = TonemapAces(hdr);
  } else if (push.tonemap == 1u) {
    ldr = hdr / (1.0 + hdr);
  } else {
    ldr = saturate(hdr);
  }
  return float4(SrgbEncode(ldr), 1.0);
}
