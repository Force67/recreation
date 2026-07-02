#include "rhi_bindings.hlsli"
// Aerial perspective: the atmospheric scattering between the camera and each
// lit surface, so distant geometry picks up the same haze/blue-shift as the sky
// (Hillaire 2020). A short raymarch camera->surface accumulates in-scattering
// (single + multiple, from the LUTs) and extinction, then composites over the
// lit scene. Sky pixels (far plane) pass through untouched - they already carry
// the full atmosphere from the sky pass.

#include "atmosphere.hlsli"

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> out_image : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D color_in : register(t1, space0);  // lit scene, fetched per texel
[[vk::binding(2, 0)]] Texture2D depth_in : register(t2, space0);  // raw reversed-z depth export
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] Texture2D<float4> transmittance : register(t3, space0);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] SamplerState transmittance_sampler : register(s3, space0);
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]] Texture2D<float4> multiscatter : register(t4, space0);
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]] SamplerState multiscatter_sampler : register(s4, space0);

struct PushData {
  column_major float4x4 inv_view_proj;
  float4 camera_pos;     // xyz eye (metres), w = effect strength
  float4 sun_direction;  // xyz travel direction, w intensity
  float4 sun_color;      // rgb
  uint2 size;
  uint steps;
  uint pad;
};
PUSH_CONSTANTS(PushData, pc);

float3 SampleTransmittanceLut(float radius, float mu) {
  return transmittance.SampleLevel(transmittance_sampler, TransmittanceUv(radius, mu), 0).rgb;
}
float3 SampleMultiScatterLut(float radius, float sun_cos) {
  return multiscatter.SampleLevel(multiscatter_sampler, MultiScatterUv(radius, sun_cos), 0).rgb;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= pc.size.x || id.y >= pc.size.y) return;
  int2 px = int2(id.xy);
  float3 color = color_in.Load(int3(px, 0)).rgb;
  float depth = depth_in.Load(int3(px, 0)).r;
  // Reversed-z: the far plane (sky) is 0. Sky already has full atmosphere.
  if (depth <= 0.0) {
    out_image[px] = float4(color, 1.0);
    return;
  }

  // Reconstruct the world-space surface and the camera->surface segment.
  float2 ndc = (float2(px) + 0.5) / float2(pc.size) * 2.0 - 1.0;
  float4 world_h = mul(pc.inv_view_proj, float4(ndc, depth, 1.0));
  float3 world = world_h.xyz / world_h.w;
  float3 cam = pc.camera_pos.xyz;
  float3 view = world - cam;
  float dist = length(view);
  if (dist < 1e-3) {
    out_image[px] = float4(color, 1.0);
    return;
  }
  view /= dist;

  float3 to_sun = normalize(-pc.sun_direction.xyz);
  // Atmosphere camera on the radial (+y) axis at the eye altitude; y is up in
  // both the scene and the atmosphere frame, so the view direction transfers.
  float3 p0 = float3(0.0, kGroundRadius + max(cam.y, 0.0) + 1.0, 0.0);

  float mu = dot(view, to_sun);
  float rayleigh_phase = RayleighPhase(mu);
  float mie_phase = MiePhase(mu, 0.8);

  float3 L = 0.0.xxx;
  float3 throughput = 1.0.xxx;
  uint steps = max(pc.steps, 1u);
  float dt = dist / float(steps);
  for (uint s = 0; s < steps; ++s) {
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

    float3 single = (rayleigh_s * rayleigh_phase + mie_s * mie_phase) * sun_trans * shadow;
    float3 multi = SampleMultiScatterLut(r, sun_mu) * m.scattering;
    float3 S = single + multi;

    L += throughput * (S - S * sample_trans) * inv_ext;
    throughput *= sample_trans;
  }
  L *= pc.sun_color.rgb * pc.sun_direction.w;

  // Composite: attenuate the surface by the camera->surface transmittance and
  // add the in-scattered light. `strength` scales the whole effect.
  float strength = pc.camera_pos.w;
  float3 result = color * lerp(float3(1, 1, 1), throughput, strength) + L * strength;
  out_image[px] = float4(result, 1.0);
}
