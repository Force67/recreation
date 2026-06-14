// Accumulates the weighted, premultiplied colour and the (1 - alpha) product for
// the WBOIT resolve. accum blends additively (src ONE, dst ONE); revealage
// blends (src ZERO, dst ONE_MINUS_SRC_COLOR) so it ends up as the transmittance.
struct PushData {
  column_major float4x4 view_proj;
  column_major float4x4 model;
  float4 color;
  float3 sun_dir;
  float pad0;
  float3 sun_color;
  float ambient;
};
[[vk::push_constant]] PushData push;

struct PsIn {
  float4 pos : SV_Position;
  [[vk::location(0)]] float3 normal : NORMAL;
  [[vk::location(1)]] float view_z : TEXCOORD0;
};

struct PsOut {
  float4 accum : SV_Target0;
  float revealage : SV_Target1;
};

PsOut main(PsIn input) {
  float3 n = normalize(input.normal);
  float ndl = saturate(dot(n, -normalize(push.sun_dir)));
  float3 lit = push.color.rgb * (ndl * push.sun_color + push.ambient);
  float a = push.color.a;

  // McGuire weight function: nearer, more opaque fragments contribute more.
  float z = input.view_z;
  float w = a * clamp(10.0 / (1e-5 + pow(z / 5.0, 2.0) + pow(z / 200.0, 6.0)), 1e-2, 3e3);

  PsOut o;
  o.accum = float4(lit * a, a) * w;
  o.revealage = a;
  return o;
}
