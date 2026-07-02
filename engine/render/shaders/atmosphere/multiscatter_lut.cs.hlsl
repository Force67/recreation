#include "rhi_bindings.hlsli"
// Hillaire 2020 multiple-scattering LUT. The O(1) approximation: estimate the
// second-order in-scattered (isotropic) luminance L2 and the transfer factor
// f_ms by integrating 64 directions over the sphere, then sum the whole infinite
// series of scattering orders in closed form, psi_ms = L2 / (1 - f_ms). This is
// the low-frequency term single scattering omits (dark horizon / flat twilight).
// Sun-independent (parameterised by sun-zenith cosine), so it bakes once. 32x32.

#include "atmosphere.hlsli"

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> out_lut : register(u0, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D<float4> transmittance : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState transmittance_sampler : register(s1, space0);

struct PushData {
  float2 size;
};
PUSH_CONSTANTS(PushData, push);

float3 SampleTransmittanceLut(float radius, float mu) {
  return transmittance.SampleLevel(transmittance_sampler, TransmittanceUv(radius, mu), 0).rgb;
}

static const int kSqrt = 8;   // 8x8 = 64 sphere directions
static const int kSteps = 20;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= (uint)push.size.x || id.y >= (uint)push.size.y) return;
  float2 uv = (float2(id.xy) + 0.5) / push.size;
  float sun_cos = uv.x * 2.0 - 1.0;
  float radius = lerp(kGroundRadius + 1.0, kTopRadius, uv.y);
  float3 sun_dir = float3(sqrt(saturate(1.0 - sun_cos * sun_cos)), sun_cos, 0.0);
  float3 p0 = float3(0.0, radius, 0.0);

  float3 lum_total = 0.0.xxx;  // sum of single-scatter over sphere directions
  float3 fms_total = 0.0.xxx;  // sum of the transfer factor over directions
  const int N = kSqrt * kSqrt;

  for (int i = 0; i < kSqrt; ++i) {
    for (int j = 0; j < kSqrt; ++j) {
      // Uniform sphere direction.
      float a = (float(i) + 0.5) / float(kSqrt);
      float b = (float(j) + 0.5) / float(kSqrt);
      float theta = 2.0 * kPi * a;
      float cos_p = 1.0 - 2.0 * b;
      float sin_p = sqrt(saturate(1.0 - cos_p * cos_p));
      float3 dir = float3(sin_p * cos(theta), cos_p, sin_p * sin(theta));

      float t_top = RaySphere(p0, dir, kTopRadius);
      float t_ground = RaySphere(p0, dir, kGroundRadius);
      float t_max = (t_ground > 0.0) ? t_ground : t_top;
      if (t_max <= 0.0) continue;

      float dt = t_max / float(kSteps);
      float3 throughput = 1.0.xxx;
      float3 L = 0.0.xxx;
      float3 fms = 0.0.xxx;
      for (int s = 0; s < kSteps; ++s) {
        float3 pos = p0 + dir * ((float(s) + 0.5) * dt);
        float r = length(pos);
        float h = r - kGroundRadius;
        Medium m = SampleMedium(h);
        float3 sample_trans = exp(-m.extinction * dt);
        float3 inv_ext = 1.0 / max(m.extinction, float3(1e-9, 1e-9, 1e-9));

        // Single scatter from the sun (isotropic phase, unit sun illuminance).
        float sun_mu = dot(normalize(pos), sun_dir);
        float3 sun_trans = SampleTransmittanceLut(r, sun_mu);
        float shadow = RaySphere(pos, sun_dir, kGroundRadius) >= 0.0 ? 0.0 : 1.0;
        float3 S = m.scattering * kUniformPhase * sun_trans * shadow;
        L += throughput * (S - S * sample_trans) * inv_ext;

        // Transfer of a uniform unit luminance field (how much re-scatters in).
        float3 Sf = m.scattering;
        fms += throughput * (Sf - Sf * sample_trans) * inv_ext;

        throughput *= sample_trans;
      }
      lum_total += L;
      fms_total += fms;
    }
  }

  float3 l2 = lum_total / float(N);
  float3 r = fms_total / float(N);
  float3 psi = l2 / max(1.0.xxx - r, float3(1e-4, 1e-4, 1e-4));
  out_lut[id.xy] = float4(psi, 1.0);
}
