// Depth-only cascade shadow caster. Renders opaque geometry from the sun into
// one sub-rect of the cascade atlas. The uv is passed through so the fragment
// stage can alpha-test masked materials (perforated foliage shadows); fully
// opaque casters never sample it. One draw per cascade, the light matrix
// arrives per draw.
struct PushData {
  column_major float4x4 light_view_proj;
  column_major float4x4 model;
};
[[vk::push_constant]] PushData push;

struct VsIn {
  [[vk::location(0)]] float3 position : POSITION;
  [[vk::location(3)]] float2 uv : TEXCOORD0;
};

struct VsOut {
  float4 pos : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;
};

VsOut main(VsIn input) {
  float4 world = mul(push.model, float4(input.position, 1.0));
  VsOut o;
  o.pos = mul(push.light_view_proj, world);
  o.uv = input.uv;
  return o;
}
