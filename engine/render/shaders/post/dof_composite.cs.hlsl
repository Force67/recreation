#include "rhi_bindings.hlsli"
// Depth of field, stage 3: full-res composite. Blends the half-res gathered
// blur over the sharp image by the pixel's own coc, with a small ramp so the
// in-focus zone stays bit-exact.
struct CompositePush {
  uint2 size;  // full res
  float2 inv_size;
};
PUSH_CONSTANTS(CompositePush, pc);

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> out_color : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float4> sharp : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture2D blurred : register(t2, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState blurred_sampler : register(s2, space0);
[[vk::binding(3, 0)]] Texture2D<float> coc : register(t3, space0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= pc.size.x || id.y >= pc.size.y) return;
  float2 uv = (float2(id.xy) + 0.5) * pc.inv_size;
  float4 s = sharp.Load(int3(id.xy, 0));
  float4 b = blurred.SampleLevel(blurred_sampler, uv, 0.0);
  float c = abs(coc.Load(int3(id.xy, 0)));
  // Near-field spill: the gather's alpha carries blur presence, so a sharp
  // pixel in front of a blurred foreground still receives the bleed.
  float blend = saturate(max(c - 0.5, b.a * 2.0 - 0.6));
  out_color[id.xy] = float4(lerp(s.rgb, b.rgb, saturate(blend)), s.a);
}
