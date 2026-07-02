#include "rhi_bindings.hlsli"
// Split-sum BRDF integration LUT (Karis), generated once at startup.
// x: ndv, y: roughness; output scale and bias for f0.

[[vk::image_format("rg16f")]] [[vk::binding(0, 0)]] RWTexture2D<float2> lut : register(u0, space0);

struct PushData {
  float size;
};
PUSH_CONSTANTS(PushData, push);

static const float kPi = 3.14159265359;
static const uint kSamples = 1024;

float2 Hammersley(uint i, uint count) {
  uint bits = reversebits(i);
  return float2((i + 0.5) / count, bits * 2.3283064365386963e-10);
}

float3 ImportanceGgx(float2 u, float roughness) {
  float a = roughness * roughness;
  float phi = 2.0 * kPi * u.x;
  float cos_theta = sqrt((1.0 - u.y) / (1.0 + (a * a - 1.0) * u.y));
  float sin_theta = sqrt(1.0 - cos_theta * cos_theta);
  return float3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);
}

float GeometrySmithIbl(float ndv, float ndl, float roughness) {
  float k = roughness * roughness / 2.0;
  float gv = ndv / (ndv * (1.0 - k) + k);
  float gl = ndl / (ndl * (1.0 - k) + k);
  return gv * gl;
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
  uint size = (uint)push.size;
  if (id.x >= size || id.y >= size) return;
  float ndv = max((id.x + 0.5) / push.size, 1e-3);
  float roughness = (id.y + 0.5) / push.size;

  float3 v = float3(sqrt(1.0 - ndv * ndv), 0.0, ndv);
  float scale = 0.0;
  float bias = 0.0;
  for (uint i = 0; i < kSamples; ++i) {
    float3 h = ImportanceGgx(Hammersley(i, kSamples), roughness);
    float3 l = normalize(2.0 * dot(v, h) * h - v);
    if (l.z > 0.0) {
      float g = GeometrySmithIbl(ndv, l.z, roughness);
      float g_vis = g * max(dot(v, h), 0.0) / (max(h.z, 1e-4) * ndv);
      float fc = pow(1.0 - max(dot(v, h), 0.0), 5.0);
      scale += (1.0 - fc) * g_vis;
      bias += fc * g_vis;
    }
  }
  lut[id] = float2(scale, bias) / kSamples;
}
