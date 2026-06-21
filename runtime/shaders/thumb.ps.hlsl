// Asset-thumbnail pixel stage. Two-sided clay lambert against a transparent
// background, so the rendered preview composites cleanly over an asset card.

struct PushData {
  column_major float4x4 mvp;
  float4 albedo;
  float4 light;
};
[[vk::push_constant]] PushData push;

struct PsIn {
  float4 sv_position : SV_Position;
  [[vk::location(0)]] float3 normal : NORMAL;
};

float4 main(PsIn input) : SV_Target0 {
  float3 n = normalize(input.normal);
  float3 l = normalize(push.light.xyz);
  // Light both faces (the mesh is drawn without back-face culling) so thin or
  // inverted-winding props still read clearly.
  float ndl = max(max(dot(n, l), 0.0), max(dot(-n, l), 0.0) * 0.6);
  float lit = 0.30 + 0.72 * ndl;
  return float4(push.albedo.rgb * lit, 1.0);
}
