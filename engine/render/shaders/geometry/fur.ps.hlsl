#include "rhi_bindings.hlsli"
// Carves hair strands out of each fur shell. A high-frequency grid over the uv
// seeds one strand per cell with a random length and a coverage gap; a fragment
// survives only while its shell height is below the local strand length, so the
// stack of shells forms tapering strands. Root-darkened and sun lit.
struct PushData {
  column_major float4x4 view_proj;
  column_major float4x4 model;
  float3 sun_dir;
  float fur_length;
  float3 sun_color;
  uint shell_count;
  float3 base_color;
  float ambient;
};
PUSH_CONSTANTS(PushData, push);

struct PsIn {
  float4 pos : SV_Position;
  [[vk::location(0)]] float3 normal : NORMAL;
  [[vk::location(1)]] float2 uv : TEXCOORD0;
  [[vk::location(2)]] float layer : TEXCOORD1;
};

float2 Hash2(float2 c) {
  float3 p = float3(c.xy, c.x * 0.731 + c.y * 1.139);
  p = frac(p * float3(443.897, 441.423, 437.195));
  p += dot(p, p.yzx + 19.19);
  return frac((p.xx + p.yz) * p.zy);
}

float4 main(PsIn input) : SV_Target {
  const float kDensity = 230.0;  // strands per uv unit
  float2 cell = floor(input.uv * kDensity);
  float2 h = Hash2(cell);

  // Coverage gap: cells below the threshold carry no strand (skin shows). The
  // skin shell (layer ~0) always survives so the base stays solid.
  if (input.layer > 0.02 && h.x > 0.72) discard;
  float strand_len = 0.4 + h.y * 0.6;
  if (input.layer > strand_len) discard;

  float tip = saturate((strand_len - input.layer) / 0.4);  // fade to the tip
  float ao = lerp(0.35, 1.0, input.layer);                 // darker toward the roots
  float3 n = normalize(input.normal);
  float ndl = saturate(dot(n, -normalize(push.sun_dir)));
  float3 lit = push.base_color * (ndl * push.sun_color + push.ambient) * ao;

  float alpha = input.layer < 0.02 ? 1.0 : tip * tip;
  return float4(lit, alpha);
}
