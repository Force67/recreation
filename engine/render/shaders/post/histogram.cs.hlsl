// Log luminance histogram of the scene, 256 bins accumulated through
// groupshared then atomically merged.

[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D scene;
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState scene_sampler;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> histogram;

struct PushData {
  float min_log_luma;
  float inv_log_luma_range;
  uint width;
  uint height;
};
[[vk::push_constant]] PushData push;

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
    InterlockedAdd(bins[bin], 1u);
  }
  GroupMemoryBarrierWithGroupSync();

  if (bins[group_index] > 0) InterlockedAdd(histogram[group_index], bins[group_index]);
}
