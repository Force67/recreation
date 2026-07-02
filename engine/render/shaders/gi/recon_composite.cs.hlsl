#include "rhi_bindings.hlsli"
// SVGF reconstruction, stage 4: composite. final = albedo/pi * denoisedIrradiance
// + emissive (the irradiance was demodulated, so re-modulating here keeps texture
// detail crisp). debug_mode swaps the output for one of the guide's debug views so
// the pipeline can be tuned by sight, not blind.
struct ReconCompositePush {
  uint2 size;
  uint debug_mode;  // 0 final,1 irradiance,2 history,3 variance,4 motion,5 normal,6 albedo,7 specular
  float max_history;
  uint fog;  // 1 = apply the volumetric fog target (scene * a + rgb)
  uint pad0;
  uint pad1;
  uint pad2;
};
PUSH_CONSTANTS(ReconCompositePush, pc);

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> out_color : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float4> albedo : register(t1, space0);
[[vk::binding(2, 0)]] Texture2D<float4> irradiance : register(t2, space0);
[[vk::binding(3, 0)]] Texture2D<float4> emissive : register(t3, space0);
[[vk::binding(4, 0)]] Texture2D<float4> moments : register(t4, space0);
[[vk::binding(5, 0)]] Texture2D<float4> normal_rough : register(t5, space0);
[[vk::binding(6, 0)]] Texture2D<float2> motion : register(t6, space0);
[[vk::binding(7, 0)]] Texture2D<float4> specular : register(t7, space0);  // denoised reflection
// Half-res fog (rgb inscatter, a transmittance), bilinearly upsampled.
[[vk::combinedImageSampler]] [[vk::binding(8, 0)]] Texture2D<float4> fog_tex : register(t8, space0);
[[vk::combinedImageSampler]] [[vk::binding(8, 0)]] SamplerState fog_sampler : register(s8, space0);

static const float kInvPi = 0.31830988618;

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  int2 p = int2(tid.xy);
  if (any(p >= int2(pc.size))) return;

  float3 alb = albedo.Load(int3(p, 0)).rgb;
  float3 e = irradiance.Load(int3(p, 0)).rgb;
  float3 em = emissive.Load(int3(p, 0)).rgb;
  float3 spec = specular.Load(int3(p, 0)).rgb;  // already F*weight*radiance, just add
  float3 color = alb * kInvPi * e + em + spec;
  if (pc.fog != 0u) {
    float2 uv = (float2(p) + 0.5) / float2(pc.size);
    float4 fog = fog_tex.SampleLevel(fog_sampler, uv, 0.0);
    color = color * fog.a + fog.rgb;
  }

  if (pc.debug_mode == 1u || pc.debug_mode >= 8u) {
    color = e * kInvPi;  // denoised lighting (albedo-free; 8/9 = restir M/W)
  } else if (pc.debug_mode == 2u) {
    float h = moments.Load(int3(p, 0)).w / max(pc.max_history, 1.0);
    color = lerp(float3(1, 0, 0), float3(0, 1, 0), saturate(h));  // red=fresh, green=full
  } else if (pc.debug_mode == 3u) {
    color = sqrt(max(moments.Load(int3(p, 0)).z, 0.0)).xxx;  // stddev
  } else if (pc.debug_mode == 4u) {
    float2 mv = motion.Load(int3(p, 0));
    color = float3(0.5 + mv.x * 50.0, 0.5 + mv.y * 50.0, 0.5);
  } else if (pc.debug_mode == 5u) {
    color = normal_rough.Load(int3(p, 0)).xyz;  // encoded 0..1
  } else if (pc.debug_mode == 6u) {
    color = alb;
  } else if (pc.debug_mode == 7u) {
    color = spec;  // denoised specular reflection only
  }

  out_color[p] = float4(color, 1.0);
}
