#include "rhi_bindings.hlsli"
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D scene : register(t0, space0);
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState scene_sampler : register(s0, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D bloom : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState bloom_sampler : register(s1, space0);
[[vk::binding(2, 0)]] StructuredBuffer<float> exposure_buffer;  // [0] resolved exposure
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] Texture2D color_lut;  // 1024x32 strip lut
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] SamplerState color_lut_sampler : register(s3, space0);

struct PushData {
  uint tonemap;  // 0 aces, 1 reinhard, 2 none
  float bloom_intensity;
  uint bloom_enabled;
  uint lut_enabled;
};
PUSH_CONSTANTS(PushData, push);

// Strip color lut: 32 blue slices laid out horizontally (1024x32). Hardware
// bilinear covers red/green within a slice; blue is a manual lerp across slices.
float3 ApplyColorLut(float3 c) {
  const float size = 32.0;
  c = saturate(c);
  float blue = c.b * (size - 1.0);
  float slice = floor(blue);
  float frac_b = blue - slice;
  float u = (c.r * (size - 1.0) + 0.5) / (size * size);
  float v = (c.g * (size - 1.0) + 0.5) / size;
  float3 a = color_lut.SampleLevel(color_lut_sampler, float2(u + slice / size, v), 0.0).rgb;
  float3 b = color_lut.SampleLevel(color_lut_sampler,
                                   float2(u + min(slice + 1.0, size - 1.0) / size, v), 0.0).rgb;
  return lerp(a, b, frac_b);
}

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
  float3 hdr = scene.Sample(scene_sampler, uv).rgb;
  if (push.bloom_enabled != 0u) {
    hdr = lerp(hdr, bloom.Sample(bloom_sampler, uv).rgb, push.bloom_intensity);
  }
  hdr *= exposure_buffer[0];

  float3 ldr;
  if (push.tonemap == 0u) {
    ldr = TonemapAces(hdr);
  } else if (push.tonemap == 1u) {
    ldr = hdr / (1.0 + hdr);
  } else {
    ldr = saturate(hdr);
  }
  if (push.lut_enabled != 0u) ldr = ApplyColorLut(ldr);
  return float4(SrgbEncode(ldr), 1.0);
}
