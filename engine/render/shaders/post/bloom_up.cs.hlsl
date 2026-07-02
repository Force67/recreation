#include "rhi_bindings.hlsli"
// 3x3 tent upsample, added on top of the matching downsample mip.

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> dst : register(u0, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D src : register(t1, space0);  // smaller mip
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState src_sampler : register(s1, space0);

struct PushData {
  float2 src_inv_size;
  uint replace;  // first up into a fresh target overwrites instead of adding
  float pad;
};
PUSH_CONSTANTS(PushData, push);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint width, height;
  dst.GetDimensions(width, height);
  if (id.x >= width || id.y >= height) return;
  float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
  float2 t = push.src_inv_size;

  float3 sum = src.SampleLevel(src_sampler, uv + t * float2(-1, -1), 0).rgb;
  sum += src.SampleLevel(src_sampler, uv + t * float2(0, -1), 0).rgb * 2.0;
  sum += src.SampleLevel(src_sampler, uv + t * float2(1, -1), 0).rgb;
  sum += src.SampleLevel(src_sampler, uv + t * float2(-1, 0), 0).rgb * 2.0;
  sum += src.SampleLevel(src_sampler, uv, 0).rgb * 4.0;
  sum += src.SampleLevel(src_sampler, uv + t * float2(1, 0), 0).rgb * 2.0;
  sum += src.SampleLevel(src_sampler, uv + t * float2(-1, 1), 0).rgb;
  sum += src.SampleLevel(src_sampler, uv + t * float2(0, 1), 0).rgb * 2.0;
  sum += src.SampleLevel(src_sampler, uv + t * float2(1, 1), 0).rgb;

  float3 base = push.replace != 0u ? 0.0.xxx : dst[id.xy].rgb;
  dst[id.xy] = float4(base + sum / 16.0, 1.0);
}
