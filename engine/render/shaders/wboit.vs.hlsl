// Weighted-blended order-independent transparency (McGuire & Bavoil 2013). The
// transparent geometry accumulates premultiplied colour weighted by depth into
// one target and the product of (1 - alpha) into another, so the result is
// independent of draw order. One instance per draw, parameters in the push.
struct PushData {
  column_major float4x4 view_proj;
  column_major float4x4 model;
  float4 color;  // rgb tint, a = alpha
  float3 sun_dir;
  float pad0;
  float3 sun_color;
  float ambient;
};
[[vk::push_constant]] PushData push;

struct VsIn {
  [[vk::location(0)]] float3 position : POSITION;
  [[vk::location(1)]] float3 normal : NORMAL;
};

struct VsOut {
  float4 pos : SV_Position;
  [[vk::location(0)]] float3 normal : NORMAL;
  [[vk::location(1)]] float view_z : TEXCOORD0;  // for the depth weight
};

VsOut main(VsIn input) {
  float4 world = mul(push.model, float4(input.position, 1.0));
  float4 clip = mul(push.view_proj, world);
  VsOut o;
  o.pos = clip;
  o.normal = mul((float3x3)push.model, input.normal);
  o.view_z = clip.w;  // perspective w == view-space depth
  return o;
}
