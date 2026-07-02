#include "rhi_bindings.hlsli"
// Linear-filtered fullscreen copy, the d3d12 backend's BlitMip lowering
// (D3D12 has no filtered image copy). Pairs with fullscreen.vs.
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D src : register(t0, space0);
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState src_sampler : register(s0, space0);

struct PsIn {
  float4 sv_position : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;
};

float4 main(PsIn input) : SV_Target0 {
  return src.SampleLevel(src_sampler, input.uv, 0.0);
}
