#include "rhi_bindings.hlsli"
// Mean of the log luminance histogram (bin 0, pure black, ignored) and
// exponential adaptation toward the keyed exposure.

[[vk::binding(0, 0)]] RWStructuredBuffer<uint> histogram : register(u0, space0);
[[vk::binding(1, 0)]] RWStructuredBuffer<float> exposure : register(u1, space0);  // [0] exposure, [1] avg luma

struct PushData {
  float min_log_luma;
  float log_luma_range;
  float delta_seconds;
  float adaptation_speed;
  float compensation;  // manual multiplier on top of the metered exposure
  uint auto_exposure;
  float manual_exposure;
  float pixel_count;
};
PUSH_CONSTANTS(PushData, push);

groupshared uint bins[256];

[numthreads(256, 1, 1)]
void main(uint group_index : SV_GroupIndex) {
  uint count = histogram[group_index];
  bins[group_index] = count * group_index;
  histogram[group_index] = 0;  // cleared for the next frame
  GroupMemoryBarrierWithGroupSync();

  [unroll]
  for (uint cutoff = 128; cutoff > 0; cutoff >>= 1) {
    if (group_index < cutoff) bins[group_index] += bins[group_index + cutoff];
    GroupMemoryBarrierWithGroupSync();
  }

  if (group_index != 0) return;

  if (push.auto_exposure == 0u) {
    exposure[0] = push.manual_exposure;
    exposure[1] = 0.0;
    return;
  }

  float black = (float)count;  // thread 0 read bin 0 before the reduction
  float weighted = (float)bins[0];  // sum(count_i * i), zero-luma bin excluded
  float n = max(push.pixel_count - black, 1.0);
  float mean_bin = weighted / n;
  float mean_log_luma = push.min_log_luma + ((mean_bin - 1.0) / 254.0) * push.log_luma_range;
  float avg_luma = exp2(mean_log_luma);

  float target = push.compensation * 0.18 / clamp(avg_luma, 0.001, 1000.0);
  float current = exposure[0];
  if (current <= 0.0) current = target;
  float blend = 1.0 - exp(-push.delta_seconds * push.adaptation_speed);
  exposure[0] = lerp(current, target, blend);
  exposure[1] = avg_luma;
}
