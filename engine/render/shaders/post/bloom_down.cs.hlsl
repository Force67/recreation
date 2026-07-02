#include "rhi_bindings.hlsli"
// 13-tap downsample (CoD: Advanced Warfare). The first pass from the scene
// applies a Karis average per quad to kill fireflies before they bloom.

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> dst : register(u0, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D src : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState src_sampler : register(s1, space0);

struct PushData {
  float2 src_inv_size;
  uint first_pass;
  float pad;
};
PUSH_CONSTANTS(PushData, push);

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float3 KarisAverage(float3 a, float3 b, float3 c, float3 d) {
  float wa = 1.0 / (1.0 + Luma(a));
  float wb = 1.0 / (1.0 + Luma(b));
  float wc = 1.0 / (1.0 + Luma(c));
  float wd = 1.0 / (1.0 + Luma(d));
  return (a * wa + b * wb + c * wc + d * wd) / (wa + wb + wc + wd);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint width, height;
  dst.GetDimensions(width, height);
  if (id.x >= width || id.y >= height) return;
  float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
  float2 t = push.src_inv_size;

  float3 a = src.SampleLevel(src_sampler, uv + t * float2(-2, -2), 0).rgb;
  float3 b = src.SampleLevel(src_sampler, uv + t * float2(0, -2), 0).rgb;
  float3 c = src.SampleLevel(src_sampler, uv + t * float2(2, -2), 0).rgb;
  float3 d = src.SampleLevel(src_sampler, uv + t * float2(-2, 0), 0).rgb;
  float3 e = src.SampleLevel(src_sampler, uv, 0).rgb;
  float3 f = src.SampleLevel(src_sampler, uv + t * float2(2, 0), 0).rgb;
  float3 g = src.SampleLevel(src_sampler, uv + t * float2(-2, 2), 0).rgb;
  float3 h = src.SampleLevel(src_sampler, uv + t * float2(0, 2), 0).rgb;
  float3 i = src.SampleLevel(src_sampler, uv + t * float2(2, 2), 0).rgb;
  float3 j = src.SampleLevel(src_sampler, uv + t * float2(-1, -1), 0).rgb;
  float3 k = src.SampleLevel(src_sampler, uv + t * float2(1, -1), 0).rgb;
  float3 l = src.SampleLevel(src_sampler, uv + t * float2(-1, 1), 0).rgb;
  float3 m = src.SampleLevel(src_sampler, uv + t * float2(1, 1), 0).rgb;

  float3 result;
  if (push.first_pass != 0u) {
    result = KarisAverage(j, k, l, m) * 0.5;
    result += KarisAverage(a, b, d, e) * 0.125;
    result += KarisAverage(b, c, e, f) * 0.125;
    result += KarisAverage(d, e, g, h) * 0.125;
    result += KarisAverage(e, f, h, i) * 0.125;
  } else {
    result = e * 0.125;
    result += (a + c + g + i) * 0.03125;
    result += (b + d + f + h) * 0.0625;
    result += (j + k + l + m) * 0.125;
  }
  dst[id.xy] = float4(result, 1.0);
}
