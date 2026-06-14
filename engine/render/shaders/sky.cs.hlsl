// Single scattering rayleigh + mie atmosphere, rendered into a small cubemap
// whenever the sun changes. Cheap enough to not bother caching transmittance.

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2DArray<float4> sky_out;

struct PushData {
  float3 sun_direction;  // travel direction of the light
  float intensity;
  float3 sun_color;
  float face_size;
};
[[vk::push_constant]] PushData push;

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

static const float kPlanetRadius = 6371e3;
static const float kAtmosphereRadius = 6471e3;
static const float3 kRayleighScatter = float3(5.802e-6, 13.558e-6, 33.1e-6);
static const float kMieScatter = 3.996e-6;
static const float kMieAbsorb = 4.4e-6;
static const float kRayleighHeight = 8500.0;
static const float kMieHeight = 1200.0;
static const int kViewSamples = 24;
static const int kSunSamples = 8;
static const float kPi = 3.14159265359;

// Distance to a sphere centered at the origin, -1 when missed.
float RaySphere(float3 origin, float3 dir, float radius) {
  float b = dot(origin, dir);
  float c = dot(origin, origin) - radius * radius;
  float h = b * b - c;
  if (h < 0.0) return -1.0;
  h = sqrt(h);
  float t = -b - h;
  return t >= 0.0 ? t : -b + h;
}

float2 Densities(float3 p) {
  float h = max(length(p) - kPlanetRadius, 0.0);
  return float2(exp(-h / kRayleighHeight), exp(-h / kMieHeight));
}

float3 SunTransmittance(float3 p, float3 to_sun) {
  float t_top = RaySphere(p, to_sun, kAtmosphereRadius);
  if (t_top < 0.0) return 0.0.xxx;
  float2 depth = 0.0.xx;
  float dt = t_top / kSunSamples;
  for (int i = 0; i < kSunSamples; ++i) {
    depth += Densities(p + to_sun * ((i + 0.5) * dt)) * dt;
  }
  return exp(-(kRayleighScatter * depth.x + (kMieScatter + kMieAbsorb) * depth.y));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint size = (uint)push.face_size;
  if (id.x >= size || id.y >= size) return;
  float3 dir = CubeDir(id.z, (float2(id.xy) + 0.5) / push.face_size);
  float3 to_sun = normalize(-push.sun_direction);

  float3 origin = float3(0.0, kPlanetRadius + 200.0, 0.0);
  float t_max = RaySphere(origin, dir, kAtmosphereRadius);
  float t_ground = RaySphere(origin, dir, kPlanetRadius);
  if (t_ground > 0.0) t_max = t_ground;

  float mu = dot(dir, to_sun);
  float phase_r = 3.0 / (16.0 * kPi) * (1.0 + mu * mu);
  float g = 0.8;
  float phase_m = 3.0 / (8.0 * kPi) * ((1.0 - g * g) * (1.0 + mu * mu)) /
                  ((2.0 + g * g) * pow(1.0 + g * g - 2.0 * g * mu, 1.5));

  float3 sum_r = 0.0.xxx;
  float3 sum_m = 0.0.xxx;
  float2 depth = 0.0.xx;
  float dt = t_max / kViewSamples;
  for (int i = 0; i < kViewSamples; ++i) {
    float3 p = origin + dir * ((i + 0.5) * dt);
    float2 d = Densities(p) * dt;
    depth += d;
    float3 view_trans =
        exp(-(kRayleighScatter * depth.x + (kMieScatter + kMieAbsorb) * depth.y));
    float3 sun_trans = SunTransmittance(p, to_sun);
    sum_r += d.x * view_trans * sun_trans;
    sum_m += d.y * view_trans * sun_trans;
  }

  float3 sun = push.sun_color * push.intensity;
  float3 sky = (kRayleighScatter * phase_r * sum_r + kMieScatter * phase_m * sum_m) * sun;

  // Sun disk, attenuated by the full path transmittance; the bloom pass
  // gets something to flare on.
  if (t_ground < 0.0) {
    float3 trans = exp(-(kRayleighScatter * depth.x + (kMieScatter + kMieAbsorb) * depth.y));
    float cos_radius = cos(0.00465);
    float disk = smoothstep(cos_radius, cos(0.00465 * 0.6), mu);
    sky += disk * trans * sun * 100.0;
  }

  sky_out[id] = float4(sky, 1.0);
}
