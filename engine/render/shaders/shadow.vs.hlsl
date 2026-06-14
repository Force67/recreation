// Depth-only cascade shadow caster. Renders opaque geometry from the sun into
// one sub-rect of the cascade atlas; the fragment stage is omitted (depth is
// the only output). One draw per cascade, the light matrix arrives per draw.
struct PushData {
  column_major float4x4 light_view_proj;
  column_major float4x4 model;
};
[[vk::push_constant]] PushData push;

struct VsIn {
  [[vk::location(0)]] float3 position : POSITION;
};

float4 main(VsIn input) : SV_Position {
  float4 world = mul(push.model, float4(input.position, 1.0));
  return mul(push.light_view_proj, world);
}
