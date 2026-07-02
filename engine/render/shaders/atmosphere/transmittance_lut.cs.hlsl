#include "rhi_bindings.hlsli"
// Hillaire 2020 transmittance LUT: optical depth from a point to the top of the
// atmosphere, exp'd to a transmittance. Sun-independent, so it bakes once. The
// multiple-scattering LUT and the sky raymarch both sample it for sun/view
// extinction. 256x64, parameterised by (view-zenith cosine, altitude).

#include "atmosphere.hlsli"

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> out_lut : register(u0, space0);

struct PushData {
  float2 size;  // (width, height) in texels
};
PUSH_CONSTANTS(PushData, push);

static const int kSteps = 40;

float3 ComputeTransmittance(float radius, float mu) {
  // Local frame: radial direction is +y; the view ray has zenith cosine `mu`.
  float3 p = float3(0.0, radius, 0.0);
  float3 dir = float3(sqrt(max(1.0 - mu * mu, 0.0)), mu, 0.0);
  float t_top = RaySphere(p, dir, kTopRadius);
  if (t_top < 0.0) return 1.0.xxx;  // already outside / grazing: no extinction

  float3 optical_depth = 0.0.xxx;
  float dt = t_top / float(kSteps);
  for (int i = 0; i < kSteps; ++i) {
    float3 s = p + dir * ((float(i) + 0.5) * dt);
    float h = length(s) - kGroundRadius;
    optical_depth += SampleMedium(h).extinction * dt;
  }
  return exp(-optical_depth);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= (uint)push.size.x || id.y >= (uint)push.size.y) return;
  float2 uv = (float2(id.xy) + 0.5) / push.size;
  float radius, mu;
  UvToTransmittanceParams(uv, radius, mu);
  out_lut[id.xy] = float4(ComputeTransmittance(radius, mu), 1.0);
}
