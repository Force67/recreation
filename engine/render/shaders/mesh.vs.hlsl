struct FrameGlobals {
  column_major float4x4 view_proj;
  column_major float4x4 prev_view_proj;
  column_major float4x4 inv_view_proj;
  float2 jitter;  // ndc units, applied on top of the unjittered clip pos
  float2 prev_jitter;
  float4 sun_direction;  // xyz travel direction of the light, w intensity
  float4 sun_color;      // rgb color, w flat ambient when ibl is off
  float4 camera_position;  // xyz eye, w ibl intensity
  float4 misc;             // x,y render size, z sun angular radius, w frame index
  uint flags;
  float3 pad;
};
[[vk::binding(0, 0)]] ConstantBuffer<FrameGlobals> frame;

struct PushData {
  column_major float4x4 model;
  column_major float4x4 prev_model;
};
[[vk::push_constant]] PushData push;

struct VsIn {
  [[vk::location(0)]] float3 position : POSITION;
  [[vk::location(1)]] float3 normal : NORMAL;
  [[vk::location(2)]] float4 tangent : TANGENT;
  [[vk::location(3)]] float2 uv : TEXCOORD0;
  [[vk::location(4)]] float4 color : COLOR0;
};

struct VsOut {
  float4 sv_position : SV_Position;
  [[vk::location(0)]] float3 normal : NORMAL;
  [[vk::location(1)]] float4 curr_clip : TEXCOORD1;
  [[vk::location(2)]] float4 prev_clip : TEXCOORD2;
  [[vk::location(3)]] float3 world_pos : TEXCOORD3;
  [[vk::location(4)]] float4 tangent : TANGENT;
  [[vk::location(5)]] float2 uv : TEXCOORD0;
  [[vk::location(6)]] float4 color : COLOR0;
};

VsOut main(VsIn input) {
  VsOut output;
  float4 world = mul(push.model, float4(input.position, 1.0));
  float4 clip = mul(frame.view_proj, world);
  output.world_pos = world.xyz;
  // Motion vectors compare unjittered positions, so jitter only moves the
  // rasterized sample, never the reprojection.
  output.curr_clip = clip;
  output.prev_clip = mul(frame.prev_view_proj, mul(push.prev_model, float4(input.position, 1.0)));
  output.sv_position = clip + float4(frame.jitter * clip.w, 0.0, 0.0);
  output.normal = mul((float3x3)push.model, input.normal);
  output.tangent = float4(mul((float3x3)push.model, input.tangent.xyz), input.tangent.w);
  output.uv = input.uv;
  output.color = input.color;
  return output;
}
