#include "rhi_bindings.hlsli"
// Sky cubemap (Hillaire 2020): a Rayleigh + Mie + ozone atmosphere raymarched
// per cube direction, with single scattering (proper phases) PLUS the multiple-
// scattering term from the precomputed LUT and sun/view extinction from the
// transmittance LUT. Regenerated whenever the sun moves; feeds IBL, reflections
// and the on-screen sky background. The crisp sun/moon discs are drawn screen
// space in sky.ps.hlsl, so this holds scattered radiance only.

#include "atmosphere.hlsli"

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2DArray<float4> sky_out : register(u0, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D<float4> transmittance : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState transmittance_sampler : register(s1, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture2D<float4> multiscatter : register(t2, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState multiscatter_sampler : register(s2, space0);

struct PushData {
  float3 sun_direction;  // travel direction of the light
  float intensity;
  float3 sun_color;
  float face_size;
};
PUSH_CONSTANTS(PushData, push);

// Vulkan cubemap face order +x -x +y -y +z -z; uv in [0,1] within a face.
float3 CubeDir(uint face, float2 uv) {
  float2 c = uv * 2.0 - 1.0;
  float3 dir;
  if (face == 0) dir = float3(1.0, -c.y, -c.x);
  else if (face == 1) dir = float3(-1.0, -c.y, c.x);
  else if (face == 2) dir = float3(c.x, 1.0, c.y);
  else if (face == 3) dir = float3(c.x, -1.0, -c.y);
  else if (face == 4) dir = float3(c.x, -c.y, 1.0);
  else dir = float3(-c.x, -c.y, -1.0);
  return normalize(dir);
}

float3 SampleTransmittanceLut(float radius, float mu) {
  return transmittance.SampleLevel(transmittance_sampler, TransmittanceUv(radius, mu), 0).rgb;
}
float3 SampleMultiScatterLut(float radius, float sun_cos) {
  return multiscatter.SampleLevel(multiscatter_sampler, MultiScatterUv(radius, sun_cos), 0).rgb;
}

static const int kSteps = 32;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint size = (uint)push.face_size;
  if (id.x >= size || id.y >= size) return;
  float3 view = CubeDir(id.z, (float2(id.xy) + 0.5) / push.face_size);
  float3 to_sun = normalize(-push.sun_direction);

  // Eye a little above the ground on the radial (+y) axis; view and to_sun are
  // already y-up world directions, so the atmosphere is sampled consistently.
  float3 p0 = float3(0.0, kGroundRadius + 500.0, 0.0);
  float t_top = RaySphere(p0, view, kTopRadius);
  float t_ground = RaySphere(p0, view, kGroundRadius);
  float t_max = (t_ground > 0.0) ? t_ground : t_top;
  if (t_max <= 0.0) {
    sky_out[id] = float4(0.0, 0.0, 0.0, 1.0);
    return;
  }

  float mu = dot(view, to_sun);
  float rayleigh_phase = RayleighPhase(mu);
  float mie_phase = MiePhase(mu, 0.8);

  float3 L = 0.0.xxx;
  float3 throughput = 1.0.xxx;
  float dt = t_max / float(kSteps);
  for (int s = 0; s < kSteps; ++s) {
    float3 pos = p0 + view * ((float(s) + 0.5) * dt);
    float r = length(pos);
    float h = r - kGroundRadius;
    Medium m = SampleMedium(h);
    float3 sample_trans = exp(-m.extinction * dt);
    float3 inv_ext = 1.0 / max(m.extinction, float3(1e-9, 1e-9, 1e-9));

    float rayleigh = exp(-h / kRayleighHeight);
    float mie = exp(-h / kMieHeight);
    float3 rayleigh_s = kRayleighScatter * rayleigh;
    float mie_s = kMieScatter * mie;

    float sun_mu = dot(normalize(pos), to_sun);
    float3 sun_trans = SampleTransmittanceLut(r, sun_mu);
    float shadow = RaySphere(pos, to_sun, kGroundRadius) >= 0.0 ? 0.0 : 1.0;

    // Single scatter with proper Rayleigh/Mie phases + multiple scattering.
    float3 single = (rayleigh_s * rayleigh_phase + mie_s * mie_phase) * sun_trans * shadow;
    float3 multi = SampleMultiScatterLut(r, sun_mu) * m.scattering;
    float3 S = single + multi;

    L += throughput * (S - S * sample_trans) * inv_ext;
    throughput *= sample_trans;
  }

  L *= push.sun_color * push.intensity;
  sky_out[id] = float4(L, 1.0);
}
