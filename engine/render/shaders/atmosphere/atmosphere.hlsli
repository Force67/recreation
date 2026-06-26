#ifndef RECREATION_SHADERS_ATMOSPHERE_HLSLI
#define RECREATION_SHADERS_ATMOSPHERE_HLSLI

// Shared Earth-atmosphere medium model and helpers for the Hillaire 2020
// transmittance / multiple-scattering LUTs and the sky raymarch. Lengths are in
// metres. Constants follow Hillaire's sample (Rayleigh + Mie + an ozone tent),
// which is what gives a lit horizon and blue twilight that single scattering
// alone cannot. See sebh/UnrealEngineSkyAtmosphere.

static const float kPi = 3.14159265359;
static const float kGroundRadius = 6360e3;  // planet surface
static const float kTopRadius = 6460e3;     // atmosphere top (100 km shell)

static const float3 kRayleighScatter = float3(5.802e-6, 13.558e-6, 33.1e-6);
static const float kRayleighHeight = 8000.0;
static const float kMieScatter = 3.996e-6;
static const float kMieExtinction = 4.40e-6;  // scattering + absorption
static const float kMieHeight = 1200.0;
static const float3 kOzoneAbsorption = float3(0.650e-6, 1.881e-6, 0.085e-6);

struct Medium {
  float3 scattering;  // rayleigh + mie scattering coefficients (1/m)
  float3 extinction;  // scattering + absorption (+ ozone)
};

// Density-weighted medium at an altitude above the ground (metres).
Medium SampleMedium(float height_m) {
  float rayleigh = exp(-height_m / kRayleighHeight);
  float mie = exp(-height_m / kMieHeight);
  float ozone = max(0.0, 1.0 - abs(height_m - 25e3) / 15e3);  // tent peaking at 25 km

  float3 rayleigh_scatter = kRayleighScatter * rayleigh;
  float mie_scatter = kMieScatter * mie;

  Medium m;
  m.scattering = rayleigh_scatter + mie_scatter;
  m.extinction = rayleigh_scatter + kMieExtinction * mie + kOzoneAbsorption * ozone;
  return m;
}

// Nearest positive intersection of a ray from p with a sphere of `radius`
// centred at the origin; -1.0 on a miss (or fully behind).
float RaySphere(float3 p, float3 dir, float radius) {
  float b = dot(p, dir);
  float c = dot(p, p) - radius * radius;
  float disc = b * b - c;
  if (disc < 0.0) return -1.0;
  disc = sqrt(disc);
  float t0 = -b - disc;
  float t1 = -b + disc;
  if (t1 < 0.0) return -1.0;
  return t0 < 0.0 ? t1 : t0;
}

float RayleighPhase(float cos_theta) {
  return 3.0 / (16.0 * kPi) * (1.0 + cos_theta * cos_theta);
}
float MiePhase(float cos_theta, float g) {
  float g2 = g * g;
  float num = (1.0 - g2) * (1.0 + cos_theta * cos_theta);
  float denom = (2.0 + g2) * pow(abs(1.0 + g2 - 2.0 * g * cos_theta), 1.5);
  return 3.0 / (8.0 * kPi) * num / denom;
}
static const float kUniformPhase = 1.0 / (4.0 * kPi);

// Transmittance LUT parameterisation (Bruneton/Hillaire): a (radius, view-zenith
// cosine) pair maps to uv in [0,1]^2 and back. The mapping concentrates samples
// near the horizon where transmittance changes fastest.
float2 TransmittanceUv(float radius, float mu) {
  float H = sqrt(max(kTopRadius * kTopRadius - kGroundRadius * kGroundRadius, 0.0));
  float rho = sqrt(max(radius * radius - kGroundRadius * kGroundRadius, 0.0));
  float discriminant = radius * radius * (mu * mu - 1.0) + kTopRadius * kTopRadius;
  float d = max(0.0, -radius * mu + sqrt(max(discriminant, 0.0)));
  float d_min = kTopRadius - radius;
  float d_max = rho + H;
  float x_mu = (d - d_min) / (d_max - d_min);
  float x_r = rho / H;
  return float2(x_mu, x_r);
}
void UvToTransmittanceParams(float2 uv, out float radius, out float mu) {
  float H = sqrt(max(kTopRadius * kTopRadius - kGroundRadius * kGroundRadius, 0.0));
  float rho = H * uv.y;
  radius = sqrt(rho * rho + kGroundRadius * kGroundRadius);
  float d_min = kTopRadius - radius;
  float d_max = rho + H;
  float d = d_min + uv.x * (d_max - d_min);
  mu = d == 0.0 ? 1.0 : (kTopRadius * kTopRadius - radius * radius - d * d) / (2.0 * radius * d);
  mu = clamp(mu, -1.0, 1.0);
}

// Multiple-scattering LUT parameterisation: x <- sun zenith cosine in [-1,1],
// y <- normalised altitude in [ground, top].
float2 MultiScatterUv(float radius, float sun_cos) {
  float x = sun_cos * 0.5 + 0.5;
  float y = saturate((radius - kGroundRadius) / (kTopRadius - kGroundRadius));
  return float2(x, y);
}

#endif  // RECREATION_SHADERS_ATMOSPHERE_HLSLI
