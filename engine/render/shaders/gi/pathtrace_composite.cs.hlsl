#include "rhi_bindings.hlsli"
// Re-modulates the NRD-denoised diffuse radiance with the primary surface albedo
// and adds the background (sky on a primary miss + primary emissive) back in,
// producing the final path-traced scene color. Pairs with
// pathtrace_gbuffer.cs.hlsl, which demodulated the albedo before denoising.
#include "NRD.hlsli"

struct CompositePush {
  uint2 size;
};
PUSH_CONSTANTS(CompositePush, pc);

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> scene_color : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float4> denoised : register(t1, space0);  // NRD OUT_DIFF_RADIANCE_HITDIST
[[vk::binding(2, 0)]] Texture2D<float4> albedo : register(t2, space0);  // primary base color
[[vk::binding(3, 0)]] Texture2D<float4> background : register(t3, space0);  // sky on miss + primary emissive

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= pc.size.x || id.y >= pc.size.y) return;
  int3 p = int3(id.xy, 0);
  float3 lighting = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(denoised.Load(p)).rgb;
  float3 alb = albedo.Load(p).rgb;
  float3 bg = background.Load(p).rgb;
  scene_color[id.xy] = float4(lighting * alb + bg, 1.0);
}
