#include "rhi_bindings.hlsli"
// Camera-facing billboard particles, vertex-pulled from a storage buffer (no
// vertex buffer): four corners per instance expanded along the camera right/up
// axes. Lit and soft-faded in the pixel shader.
struct Particle {
  float3 pos;
  float size;
  float4 color;  // rgb tint, a opacity
  float3 prev_pos;  // last frame's centre, for the motion vector
  uint tex;         // bindless texture index, 0xffffffff = procedural sprite
};
[[vk::binding(0, 0)]] StructuredBuffer<Particle> particles : register(t0, space0);

struct PushData {
  column_major float4x4 view_proj;
  float3 cam_right;
  float near_plane;
  float3 cam_up;
  float soft_fade;
  float3 sun_dir;
  float sun_intensity;
  float3 sun_color;
  float ambient;
  column_major float4x4 prev_view_proj;
};
PUSH_CONSTANTS(PushData, push);

struct VsOut {
  float4 pos : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;  // -1..1 across the quad
  [[vk::location(1)]] float4 color : COLOR0;
  [[vk::location(2)]] float2 motion : TEXCOORD1;  // centre velocity, uv space
  [[vk::location(3)]] float3 world_pos : TEXCOORD2;  // for clustered lighting
  [[vk::location(4)]] nointerpolation uint tex : TEXCOORD3;  // bindless texture
};

static const float2 kCorners[4] = {float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, 1)};

VsOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
  Particle p = particles[iid];
  float2 c = kCorners[vid];
  float3 world = p.pos + push.cam_right * (c.x * p.size) + push.cam_up * (c.y * p.size);
  VsOut o;
  o.pos = mul(push.view_proj, float4(world, 1.0));
  o.uv = c;
  o.world_pos = world;
  o.color = p.color;
  o.tex = p.tex;
  // Centre motion, matching the prepass convention (prev - curr) * 0.5 in uv.
  float4 cc = mul(push.view_proj, float4(p.pos, 1.0));
  float4 pc = mul(push.prev_view_proj, float4(p.prev_pos, 1.0));
  o.motion = (pc.xy / pc.w - cc.xy / cc.w) * 0.5;
  return o;
}
