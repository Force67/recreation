#include "rhi_bindings.hlsli"
// Depth of field, stage 2: half-res bokeh gather. 48-tap golden-angle spiral
// scatter-as-gather: a sample contributes when its own coc reaches the pixel
// (near-field bleed over sharp backgrounds) or the pixel's coc reaches the
// sample (background blur). Weighted toward the disc edge for a bokeh-ish
// rim rather than a gaussian mush.
struct GatherPush {
  uint2 size;       // half res
  float2 inv_size;
  float max_coc;    // full-res pixels
  float pad0;
  float pad1;
  float pad2;
};
PUSH_CONSTANTS(GatherPush, pc);

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> out_color : register(u0, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D color : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState color_sampler : register(s1, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture2D<float> coc : register(t2, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState coc_sampler : register(s2, space0);

static const float kGolden = 2.39996323;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= pc.size.x || id.y >= pc.size.y) return;
  float2 uv = (float2(id.xy) + 0.5) * pc.inv_size;
  float center_coc = coc.SampleLevel(coc_sampler, uv, 0.0);
  float radius_px = abs(center_coc) * 0.5;  // half res

  float3 sum = color.SampleLevel(color_sampler, uv, 0.0).rgb;
  float weight_sum = 1.0;
  float max_r = pc.max_coc * 0.5;
  [loop]
  for (uint i = 0; i < 48u; ++i) {
    float r = sqrt((float(i) + 0.5) / 48.0);
    float ang = float(i) * kGolden;
    float2 offset_px = float2(cos(ang), sin(ang)) * r * max_r;
    float dist_px = r * max_r;
    float2 suv = uv + offset_px * pc.inv_size;
    float s_coc = coc.SampleLevel(coc_sampler, suv, 0.0);
    float s_r = abs(s_coc) * 0.5;
    // Foreground samples always spill; background samples only gather when
    // the CENTER is blurred (they must not bleed over a sharp foreground).
    float reach = s_coc < 0.0 ? s_r : min(s_r, radius_px);
    float w = saturate((reach - dist_px) * 2.0 + 1.0);
    w *= 0.5 + 0.5 * r;  // edge-weighted: bokeh rim, not gaussian
    sum += color.SampleLevel(color_sampler, suv, 0.0).rgb * w;
    weight_sum += w;
  }
  out_color[id.xy] = float4(sum / weight_sum, saturate(radius_px / max(max_r, 1e-3)));
}
