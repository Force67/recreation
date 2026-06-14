// Sky background: fullscreen triangle at depth 0 with EQUAL test so only
// untouched pixels shade. Writes camera-rotation motion vectors so temporal
// passes track the horizon.

struct FrameGlobals {
  column_major float4x4 view_proj;
  column_major float4x4 prev_view_proj;
  column_major float4x4 inv_view_proj;
  float2 jitter;
  float2 prev_jitter;
  float4 sun_direction;
  float4 sun_color;
  float4 camera_position;
  float4 misc;  // x,y render size, z sun angular radius, w frame index
  uint flags;
  float3 pad;
};
[[vk::binding(0, 0)]] ConstantBuffer<FrameGlobals> frame;

[[vk::combinedImageSampler]] [[vk::binding(0, 1)]] TextureCube sky;
[[vk::combinedImageSampler]] [[vk::binding(0, 1)]] SamplerState sky_sampler;

struct PsOut {
  float4 color : SV_Target0;
  float2 motion : SV_Target1;
};

PsOut main(float4 sv_position : SV_Position,
           [[vk::location(0)]] float2 uv : TEXCOORD0) {
  // Match the geometry jitter so temporal passes see a consistent frame.
  float2 ndc = uv * 2.0 - 1.0 + frame.jitter;
  float4 near = mul(frame.inv_view_proj, float4(ndc, 1.0, 1.0));  // reversed z near
  float3 dir = normalize(near.xyz / near.w - frame.camera_position.xyz);

  PsOut output;
  output.color = float4(sky.SampleLevel(sky_sampler, dir, 0).rgb, 1.0);

  // Reproject a point far along the ray; translation is negligible there.
  float3 far_point = frame.camera_position.xyz + dir * 1e7;
  float4 curr = mul(frame.view_proj, float4(far_point, 1.0));
  float4 prev = mul(frame.prev_view_proj, float4(far_point, 1.0));
  output.motion = (prev.xy / prev.w - curr.xy / curr.w) * 0.5;
  return output;
}
