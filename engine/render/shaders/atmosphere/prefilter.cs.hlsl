#include "rhi_bindings.hlsli"
// GGX prefiltered specular environment, one dispatch per mip with roughness
// mapped along the chain.

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2DArray<float4> prefiltered_out : register(u0, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] TextureCube sky : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState sky_sampler : register(s1, space0);

struct PushData {
  float face_size;
  float roughness;
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
static const uint kSamples = 128;

float2 Hammersley(uint i, uint count) {
  uint bits = reversebits(i);
  return float2((i + 0.5) / count, bits * 2.3283064365386963e-10);
}

float3 ImportanceGgx(float2 u, float3 n, float roughness) {
  float a = roughness * roughness;
  float phi = 2.0 * kPi * u.x;
  float cos_theta = sqrt((1.0 - u.y) / (1.0 + (a * a - 1.0) * u.y));
  float sin_theta = sqrt(1.0 - cos_theta * cos_theta);
  float3 h = float3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);
  float3 up = abs(n.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
  float3 t = normalize(cross(up, n));
  float3 b = cross(n, t);
  return normalize(t * h.x + b * h.y + n * h.z);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint size = (uint)push.face_size;
  if (id.x >= size || id.y >= size) return;
  float3 n = CubeDir(id.z, (float2(id.xy) + 0.5) / push.face_size);
  float3 v = n;

  float3 sum = 0.0.xxx;
  float weight = 0.0;
  for (uint i = 0; i < kSamples; ++i) {
    float3 h = ImportanceGgx(Hammersley(i, kSamples), n, push.roughness);
    float3 l = normalize(2.0 * dot(v, h) * h - v);
    float ndl = dot(n, l);
    if (ndl > 0.0) {
      // Disk-free: the analytic ggx sun term owns specular sun reflection.
      sum += min(sky.SampleLevel(sky_sampler, l, 0).rgb, 16.0.xxx) * ndl;
      weight += ndl;
    }
  }
  prefiltered_out[id] = float4(sum / max(weight, 1e-4), 1.0);
}
