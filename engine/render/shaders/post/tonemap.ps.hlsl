#include "rhi_bindings.hlsli"
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D scene : register(t0, space0);
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState scene_sampler : register(s0, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D bloom : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState bloom_sampler : register(s1, space0);
[[vk::binding(2, 0)]] StructuredBuffer<float> exposure_buffer : register(t2, space0);  // [0] resolved exposure
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] Texture2D color_lut : register(t3, space0);  // 1024x32 strip lut
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] SamplerState color_lut_sampler : register(s3, space0);

struct PushData {
  uint tonemap;  // 0 aces, 1 reinhard, 2 none
  float bloom_intensity;
  uint bloom_enabled;
  uint lut_enabled;
  // Output encode: 0 sRGB (SDR), 1 HDR10 PQ, 2 scRGB linear fp16. The HDR
  // modes are SDR-referred for now: the tonemapped [0,1] signal maps to
  // paper_white nits, so grading/LUTs stay valid; highlight-through HDR
  // grading is a later stage.
  uint output_transfer;
  float paper_white;  // nits of tonemapped 1.0 in the HDR modes
  float pad0;
  float pad1;
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

// Rec.709 -> Rec.2020 primaries (HDR10 containers are Rec.2020-coded).
float3 Rec709ToRec2020(float3 c) {
  return float3(dot(float3(0.6274, 0.3293, 0.0433), c),
                dot(float3(0.0691, 0.9195, 0.0114), c),
                dot(float3(0.0164, 0.0880, 0.8956), c));
}

// SMPTE ST 2084 (PQ) OETF; input in nits / 10000.
float3 PqEncode(float3 n) {
  const float m1 = 0.1593017578125, m2 = 78.84375;
  const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
  float3 p = pow(max(n, 0.0), m1);
  return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
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
  if (push.output_transfer == 1u) {  // HDR10 PQ
    float3 nits = Rec709ToRec2020(ldr) * push.paper_white;
    return float4(PqEncode(nits / 10000.0), 1.0);
  }
  if (push.output_transfer == 2u) {  // scRGB linear, 1.0 = 80 nits
    return float4(ldr * (push.paper_white / 80.0), 1.0);
  }
  return float4(SrgbEncode(ldr), 1.0);
}
