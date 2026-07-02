#include "rhi_bindings.hlsli"
[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> out_resolved : register(u0, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D scene : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState scene_sampler : register(s1, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture2D history : register(t2, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState history_sampler : register(s2, space0);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] Texture2D motion : register(t3, space0);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] SamplerState motion_sampler : register(s3, space0);
[[vk::image_format("rgba16f")]] [[vk::binding(4, 0)]] RWTexture2D<float4> debug_out : register(u4, space0);

struct PushData {
  float2 inv_size;
  float history_blend;
  uint reset_history;
  uint debug;  // 1 = output the disocclusion heatmap
};
PUSH_CONSTANTS(PushData, push);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  int2 p = int2(id.xy);
  uint width, height;
  out_resolved.GetDimensions(width, height);
  int2 size = int2(width, height);
  if (p.x >= size.x || p.y >= size.y) return;

  float3 curr = scene.Load(int3(p, 0)).rgb;

  // Motion-vector view: red = +x velocity, green = +y, grey = still. Independent
  // of the history, so it works even on the reset frame.
  if (push.debug == 2u) {
    float2 mv = motion.Load(int3(p, 0)).xy;
    debug_out[p] = float4(0.5 + mv.x * 300.0, 0.5 + mv.y * 300.0, 0.5, 1.0);
  }

  if (push.reset_history != 0u) {
    out_resolved[p] = float4(curr, 1.0);
    if (push.debug == 1u) debug_out[p] = float4(0.0, 0.05, 0.15, 1.0);  // no history yet
    return;
  }

  float2 uv = (float2(p) + 0.5) * push.inv_size;
  float2 history_uv = uv + motion.Load(int3(p, 0)).xy;
  float3 raw_hist = history.SampleLevel(history_sampler, history_uv, 0).rgb;

  // Neighborhood clamp: history outside the 3x3 color bounds of the current
  // frame is a disocclusion or lighting change, rein it in to kill ghosting.
  float3 box_min = curr;
  float3 box_max = curr;
  [unroll]
  for (int y = -1; y <= 1; ++y) {
    [unroll]
    for (int x = -1; x <= 1; ++x) {
      float3 c = scene.Load(int3(clamp(p + int2(x, y), int2(0, 0), size - 1), 0)).rgb;
      box_min = min(box_min, c);
      box_max = max(box_max, c);
    }
  }
  float3 hist = clamp(raw_hist, box_min, box_max);

  float blend = push.history_blend;
  bool offscreen = any(history_uv < 0.0) || any(history_uv > 1.0);
  if (offscreen) blend = 0.0;  // reprojected off screen, no history to use

  // Always write the real resolved color so the history ping-pong stays valid.
  out_resolved[p] = float4(lerp(curr, hist, blend), 1.0);

  if (push.debug == 1u) {
    // How far the clamp had to pull the history back into the current frame's
    // local color range: high where temporal reuse breaks (disocclusion, fast
    // motion, lighting change). Off-screen reprojection is full rejection. Goes
    // to a side target so it never feeds back into the history.
    float reject = saturate(length(raw_hist - hist) * 6.0);
    if (offscreen) reject = 1.0;
    debug_out[p] = float4(lerp(float3(0.0, 0.05, 0.15), float3(1.0, 0.1, 0.0), reject), 1.0);
  }
}
