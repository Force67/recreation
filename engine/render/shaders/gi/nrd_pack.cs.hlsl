#include "rhi_bindings.hlsli"
// Packs the engine g-buffer into NRD's guide inputs: IN_NORMAL_ROUGHNESS (world
// normal + roughness, NRD encoding) and IN_VIEWZ (linear view depth). Fed to
// both the REBLUR ao and SIGMA shadow denoisers.
#include "NRD.hlsli"

[[vk::image_format("rgb10a2")]] [[vk::binding(0, 0)]] RWTexture2D<float4> normal_roughness_out : register(u0, space0);
[[vk::image_format("r16f")]] [[vk::binding(1, 0)]] RWTexture2D<float> viewz_out : register(u1, space0);
[[vk::binding(2, 0)]] Texture2D<float4> normal_map : register(t2, space0);
[[vk::binding(3, 0)]] Texture2D<float> depth_map : register(t3, space0);

struct PushData {
  float near_plane;
  float denoising_range;
  float2 pad;
};
PUSH_CONSTANTS(PushData, push);

float3 OctDecode(float2 o) {
  float3 d = float3(o.x, 1.0 - abs(o.x) - abs(o.y), o.y);
  if (d.y < 0.0) {
    float2 sign_xz = float2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
    d.xz = (1.0 - abs(d.zx)) * sign_xz;
  }
  return normalize(d);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint width, height;
  viewz_out.GetDimensions(width, height);
  if (id.x >= width || id.y >= height) return;
  int3 p = int3(id.xy, 0);

  float depth = depth_map.Load(p);
  float3 n;
  float viewz;
  float roughness = 1.0;
  if (depth <= 0.0) {  // reversed z far plane: sky, kept out of the denoising range
    n = float3(0.0, 0.0, 1.0);
    viewz = push.denoising_range;
  } else {
    float4 nr = normal_map.Load(p);
    n = OctDecode(nr.rg);
    roughness = nr.b;  // material roughness exported by the prepass
    viewz = push.near_plane / depth;  // reversed infinite z: ndc = near / dist
  }

  viewz_out[id.xy] = viewz;
  normal_roughness_out[id.xy] = NRD_FrontEnd_PackNormalAndRoughness(n, roughness, 0.0);
}
