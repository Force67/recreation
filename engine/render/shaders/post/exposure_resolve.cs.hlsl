#include "rhi_bindings.hlsli"
// Percentile-trimmed mean of the (center-weighted) log luminance histogram
// and exponential adaptation toward the keyed exposure. Bin 0 (pure black)
// stays excluded; the trim keeps a dark corner or a bright sky from dragging
// the metering - the classic histogram-exposure win over a plain average.

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
  float low_percentile;   // metered mass below this fraction is ignored
  float high_percentile;  // ... and above this one (highlight rejection)
};
PUSH_CONSTANTS(PushData, push);

groupshared uint counts[256];

[numthreads(256, 1, 1)]
void main(uint group_index : SV_GroupIndex) {
  counts[group_index] = histogram[group_index];
  histogram[group_index] = 0;  // cleared for the next frame
  GroupMemoryBarrierWithGroupSync();

  if (group_index != 0) return;

  if (push.auto_exposure == 0u) {
    exposure[0] = push.manual_exposure;
    exposure[1] = 0.0;
    return;
  }

  // Total metered mass (weighted samples), zero-luma bin excluded.
  float total = 0.0;
  for (uint i = 1; i < 256; ++i) total += (float)counts[i];
  total = max(total, 1.0);
  float lo = total * push.low_percentile;
  float hi = total * push.high_percentile;

  // Weighted mean of the mass inside [lo, hi]; bins straddling a cut
  // contribute their inside fraction. 256 serial iterations on one thread
  // are noise next to the histogram pass itself.
  float accum = 0.0;
  float weighted = 0.0;
  float used = 0.0;
  for (uint b = 1; b < 256; ++b) {
    float c = (float)counts[b];
    float start = accum;
    accum += c;
    float inside = max(0.0, min(accum, hi) - max(start, lo));
    weighted += inside * (float)b;
    used += inside;
  }
  float mean_bin = weighted / max(used, 1.0);
  float mean_log_luma = push.min_log_luma + ((mean_bin - 1.0) / 254.0) * push.log_luma_range;
  float avg_luma = exp2(mean_log_luma);

  float target = push.compensation * 0.18 / clamp(avg_luma, 0.001, 1000.0);
  float current = exposure[0];
  if (current <= 0.0) current = target;
  float blend = 1.0 - exp(-push.delta_seconds * push.adaptation_speed);
  exposure[0] = lerp(current, target, blend);
  exposure[1] = avg_luma;
}
