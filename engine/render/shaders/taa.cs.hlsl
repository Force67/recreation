[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> out_resolved;
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D scene;
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState scene_sampler;
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture2D history;
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState history_sampler;
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] Texture2D motion;
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] SamplerState motion_sampler;

struct PushData {
  float2 inv_size;
  float history_blend;
  uint reset_history;
};
[[vk::push_constant]] PushData push;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  int2 p = int2(id.xy);
  uint width, height;
  out_resolved.GetDimensions(width, height);
  int2 size = int2(width, height);
  if (p.x >= size.x || p.y >= size.y) return;

  float3 curr = scene.Load(int3(p, 0)).rgb;
  if (push.reset_history != 0u) {
    out_resolved[p] = float4(curr, 1.0);
    return;
  }

  float2 uv = (float2(p) + 0.5) * push.inv_size;
  float2 history_uv = uv + motion.Load(int3(p, 0)).xy;
  float3 hist = history.SampleLevel(history_sampler, history_uv, 0).rgb;

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
  hist = clamp(hist, box_min, box_max);

  float blend = push.history_blend;
  if (any(history_uv < 0.0) || any(history_uv > 1.0)) {
    blend = 0.0;  // reprojected off screen, no history to use
  }
  out_resolved[p] = float4(lerp(curr, hist, blend), 1.0);
}
