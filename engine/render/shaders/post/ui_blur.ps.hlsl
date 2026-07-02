#include "rhi_bindings.hlsli"
// Separable Gaussian blur for the UI frosted-glass backdrop. Run twice
// (horizontal then vertical) over a downsampled copy of the post-tonemap
// backbuffer to produce the small blurred texture frosted panels sample. Pairs
// with fullscreen.vs (a single screen-covering triangle).

[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D src : register(t0, space0);
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState src_sampler : register(s0, space0);

struct Push {
  float2 dir;  // per-tap UV step along one axis (the other component is 0)
};
PUSH_CONSTANTS(Push, push);

struct PsIn {
  float4 sv_position : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;
};

float4 main(PsIn input) : SV_Target0 {
  // 9-tap Gaussian (sigma ~ 2). Weights sum to 1.
  const float w0 = 0.2270270270;
  const float w1 = 0.1945945946;
  const float w2 = 0.1216216216;
  const float w3 = 0.0540540541;
  const float w4 = 0.0162162162;
  float2 uv = input.uv;
  float3 c = src.Sample(src_sampler, uv).rgb * w0;
  c += src.Sample(src_sampler, uv + push.dir * 1.0).rgb * w1;
  c += src.Sample(src_sampler, uv - push.dir * 1.0).rgb * w1;
  c += src.Sample(src_sampler, uv + push.dir * 2.0).rgb * w2;
  c += src.Sample(src_sampler, uv - push.dir * 2.0).rgb * w2;
  c += src.Sample(src_sampler, uv + push.dir * 3.0).rgb * w3;
  c += src.Sample(src_sampler, uv - push.dir * 3.0).rgb * w3;
  c += src.Sample(src_sampler, uv + push.dir * 4.0).rgb * w4;
  c += src.Sample(src_sampler, uv - push.dir * 4.0).rgb * w4;
  return float4(c, 1.0);
}
