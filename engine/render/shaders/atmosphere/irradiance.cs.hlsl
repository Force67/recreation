#include "rhi_bindings.hlsli"
// Cosine convolution of the sky cubemap into a small irradiance cubemap.

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2DArray<float4> irradiance_out : register(u0, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] TextureCube sky : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState sky_sampler : register(s1, space0);

struct PushData {
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

static const float kPi = 3.14159265359;
static const uint kSamples = 256;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint size = (uint)push.face_size;
  if (id.x >= size || id.y >= size) return;
  float3 n = CubeDir(id.z, (float2(id.xy) + 0.5) / push.face_size);

  float3 up = abs(n.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
  float3 t = normalize(cross(up, n));
  float3 b = cross(n, t);

  // Fibonacci spiral over the hemisphere, cosine weighted by construction.
  float3 sum = 0.0.xxx;
  for (uint i = 0; i < kSamples; ++i) {
    float u1 = (i + 0.5) / kSamples;
    float u2 = frac(i * 0.61803398875);
    float cos_theta = sqrt(1.0 - u1);
    float sin_theta = sqrt(u1);
    float phi = 2.0 * kPi * u2;
    float3 dir = t * (cos(phi) * sin_theta) + b * (sin(phi) * sin_theta) + n * cos_theta;
    // The raw sun disk stays out of ambient: direct sun light is analytic.
    sum += min(sky.SampleLevel(sky_sampler, dir, 0).rgb, 16.0.xxx);
  }
  irradiance_out[id] = float4(sum / kSamples, 1.0);
}
