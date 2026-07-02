#include "rhi_bindings.hlsli"
// Blends the frame's probe rays into the octahedral atlases with
// hysteresis. One dispatch updates irradiance (mode 0) or filtered distance
// moments (mode 1); one thread per interior texel.

struct DdgiVolume {
  float4 origin;  // xyz grid origin, w probe spacing
  uint4 counts;   // xyz probe counts, w irradiance texel resolution
  float4 params;  // x distance texels, y hysteresis, z max ray distance, w energy scale
};

static const float kPi = 3.14159265359;

// Spherical fibonacci direction for ray i of n, rotated per frame.
float3 FibonacciDir(uint i, uint n) {
  float phi = 2.0 * kPi * frac(i * 0.61803398875);
  float cos_theta = 1.0 - (2.0 * i + 1.0) / n;
  float sin_theta = sqrt(saturate(1.0 - cos_theta * cos_theta));
  return float3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);
}

float2 OctEncode(float3 d) {
  d /= (abs(d.x) + abs(d.y) + abs(d.z));
  float2 o = d.xz;
  if (d.y < 0.0) o = (1.0 - abs(d.zx)) * float2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
  return o;
}

float3 OctDecode(float2 o) {
  float3 d = float3(o.x, 1.0 - abs(o.x) - abs(o.y), o.y);
  if (d.y < 0.0) {
    float2 sign_xz = float2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
    d.xz = (1.0 - abs(d.zx)) * sign_xz;
  }
  return normalize(d);
}

uint3 ProbeFromIndex(uint index, uint3 counts) {
  return uint3(index % counts.x, (index / counts.x) % counts.y, index / (counts.x * counts.y));
}

float3 ProbePosition(uint3 probe, DdgiVolume volume) {
  return volume.origin.xyz + float3(probe) * volume.origin.w;
}

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2DArray<float4> atlas : register(u0, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D rays : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState rays_sampler : register(s1, space0);
[[vk::binding(2, 0)]] ConstantBuffer<DdgiVolume> volume : register(b2, space0);

struct PushData {
  float4 rotation_x;
  float4 rotation_y;
  float4 rotation_z;
  uint mode;  // 0 irradiance, 1 distance
  uint ray_count;
  uint reset;
  float pad;
};
PUSH_CONSTANTS(PushData, push);


[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  float texels = push.mode == 0u ? (float)volume.counts.w : volume.params.x;
  uint stride = (uint)texels + 2;
  uint probes_x = volume.counts.x * volume.counts.z;
  if (id.x >= probes_x * stride || id.y >= volume.counts.y * stride) return;

  uint2 in_probe = uint2(id.x % stride, id.y % stride);
  // Border texels are filled by the border pass.
  if (in_probe.x == 0 || in_probe.y == 0 || in_probe.x > (uint)texels ||
      in_probe.y > (uint)texels) {
    return;
  }

  uint2 probe_xy = uint2(id.x / stride, id.y / stride);
  uint3 probe = uint3(probe_xy.x % volume.counts.x, probe_xy.y, probe_xy.x / volume.counts.x);
  uint probe_index =
      probe.x + probe.y * volume.counts.x + probe.z * volume.counts.x * volume.counts.y;

  float2 oct = (float2(in_probe) - 1.0 + 0.5) / texels * 2.0 - 1.0;
  float3 texel_dir = OctDecode(oct);

  float4 sum = 0.0.xxxx;
  float weight_sum = 0.0;
  for (uint r = 0; r < push.ray_count; ++r) {
    float3 fib = FibonacciDir(r, push.ray_count);
    float3 dir = normalize(float3(dot(push.rotation_x.xyz, fib), dot(push.rotation_y.xyz, fib),
                                  dot(push.rotation_z.xyz, fib)));
    float4 ray = rays.Load(int3(r, probe_index, 0));
    float weight;
    if (push.mode == 0u) {
      weight = max(dot(texel_dir, dir), 0.0);
      if (ray.a < 0.0) continue;  // backface, no light contribution
      if (weight > 1e-4) sum += float4(ray.rgb * weight, 0.0);
    } else {
      weight = pow(max(dot(texel_dir, dir), 0.0), 64.0);
      float dist = min(abs(ray.a), volume.params.z);
      sum += float4(dist * weight, dist * dist * weight, 0.0, 0.0);
    }
    weight_sum += weight;
  }

  float4 result;
  if (push.mode == 0u) {
    float3 irradiance = weight_sum > 1e-4 ? sum.rgb / weight_sum : 0.0.xxx;
    irradiance = sqrt(irradiance);  // perceptual encoding against banding
    result = float4(irradiance, 1.0);
  } else {
    float2 moments = weight_sum > 1e-4 ? sum.rg / weight_sum
                                       : float2(volume.params.z, volume.params.z * volume.params.z);
    result = float4(moments, 0.0, 1.0);
  }

  uint3 texel = uint3(id.xy, 0);
  if (push.reset == 0u) {
    float4 previous = atlas[texel];
    result = lerp(result, previous, volume.params.y);
  }
  atlas[texel] = result;
}
