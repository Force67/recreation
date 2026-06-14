// Camera-facing billboard particles, vertex-pulled from a storage buffer (no
// vertex buffer): four corners per instance expanded along the camera right/up
// axes. Lit and soft-faded in the pixel shader.
struct Particle {
  float3 pos;
  float size;
  float4 color;  // rgb tint, a opacity
};
[[vk::binding(0, 0)]] StructuredBuffer<Particle> particles;

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
};
[[vk::push_constant]] PushData push;

struct VsOut {
  float4 pos : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;  // -1..1 across the quad
  [[vk::location(1)]] float4 color : COLOR0;
};

static const float2 kCorners[4] = {float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, 1)};

VsOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
  Particle p = particles[iid];
  float2 c = kCorners[vid];
  float3 world = p.pos + push.cam_right * (c.x * p.size) + push.cam_up * (c.y * p.size);
  VsOut o;
  o.pos = mul(push.view_proj, float4(world, 1.0));
  o.uv = c;
  o.color = p.color;
  return o;
}
