#include "rhi_bindings.hlsli"
// Log luminance histogram of the scene, 256 bins accumulated through
// groupshared then atomically merged.

[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D scene : register(t0, space0);
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState scene_sampler : register(s0, space0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> histogram : register(u1, space0);

struct PushData {
  float min_log_luma;
  float inv_log_luma_range;
  uint width;
  uint height;
};
PUSH_CONSTANTS(PushData, push);

groupshared uint bins[256];

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint group_index : SV_GroupIndex) {
  bins[group_index] = 0;
  GroupMemoryBarrierWithGroupSync();

  if (id.x < push.width && id.y < push.height) {
    float3 color = scene.Load(int3(id.xy, 0)).rgb;
    float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
    uint bin = 0;
    if (luma > 1e-4) {
      float logged = saturate((log2(luma) - push.min_log_luma) * push.inv_log_luma_range);
      bin = (uint)(logged * 254.0 + 1.0);
    }
    // Center-weighted metering: the subject usually sits mid-frame; edges
    // (sky, ground) count for less. Fixed-point weight 1..8.
    float2 ndc = (float2(id.xy) + 0.5) / float2(push.width, push.height) * 2.0 - 1.0;
    uint weight = 1u + (uint)(7.0 * exp(-1.5 * dot(ndc, ndc)));
    InterlockedAdd(bins[bin], weight);
  }
  GroupMemoryBarrierWithGroupSync();

  if (bins[group_index] > 0) InterlockedAdd(histogram[group_index], bins[group_index]);
}
