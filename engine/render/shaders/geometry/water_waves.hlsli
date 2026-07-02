// Shared Gerstner wave field for the water surface: mesh.vs displaces the
// grid vertices with it (gated on the material water flag) and water.ps
// re-evaluates it for the analytic normal and the crest/foam factor. Both
// sides MUST use identical parameters or the shading normal detaches from
// the silhouette.
#ifndef RECREATION_WATER_WAVES_HLSLI_
#define RECREATION_WATER_WAVES_HLSLI_

// xy direction (normalized), z wavelength (m), w amplitude (m).
static const float4 kGerstner[4] = {
    float4(0.780869, 0.624695, 19.0, 0.16),
    float4(-0.599997, 0.800002, 11.0, 0.09),
    float4(0.953583, -0.301131, 6.3, 0.045),
    float4(-0.286206, -0.958164, 3.7, 0.022),
};
static const float kGerstnerChop = 0.68;  // Q: 0 = rolling sine, 1 = pinched crest
static const float kWaterTau = 6.2831853;

// Displacement of the rest position xz at time t. `normal` and `crest`
// describe the displaced surface (crest is 0 flat .. 1 pinched wave top,
// feeding the whitecap foam).
float3 GerstnerWave(float2 xz, float t, out float3 normal, out float crest) {
  float3 offset = 0.0.xxx;
  float3 n = float3(0.0, 1.0, 0.0);
  float crest_sum = 0.0;
  float amp_sum = 1e-4;
  [unroll]
  for (int i = 0; i < 4; ++i) {
    float2 d = kGerstner[i].xy;
    float wavelength = kGerstner[i].z;
    float amp = kGerstner[i].w;
    float k = kWaterTau / wavelength;
    float w = sqrt(9.81 * k);  // deep-water dispersion
    float phase = k * dot(d, xz) + w * t;
    float s = sin(phase);
    float c = cos(phase);
    float q = kGerstnerChop / (k * amp * 4.0);  // per-wave Q, stays fold-free
    offset.xz += d * (q * amp * c);
    offset.y += amp * s;
    n.xz -= d * (k * amp * c);
    n.y -= kGerstnerChop * 0.25 * (k * amp * 4.0) * s / 4.0;
    // Crests are where the surface compresses: chop pulls points toward the
    // wave top, so the jacobian shrinks right where the whitecap belongs.
    crest_sum += amp * max(s, 0.0);
    amp_sum += amp;
  }
  normal = normalize(n);
  crest = saturate((crest_sum / amp_sum - 0.62) * 3.2);
  return offset;
}

#endif  // RECREATION_WATER_WAVES_HLSLI_
