#include "rhi_bindings.hlsli"
// Dual-lobe Kajiya-Kay strand shading: a shifted primary highlight plus a
// broader, root-tinted secondary lobe along the strand tangent, wrapped
// diffuse for softness, slight root darkening for depth.
struct DrawPush {
  column_major float4x4 view_proj;
  float4 camera;
  float4 sun;        // xyz travel direction, w intensity
  float4 sun_color;
  uint points_per_strand;
  float width;
  float pad0;
  float pad1;
};
PUSH_CONSTANTS(DrawPush, pc);

struct PsIn {
  float4 pos : SV_Position;
  [[vk::location(0)]] float3 tangent : TANGENT;
  [[vk::location(1)]] float3 world_pos : POSITION1;
  [[vk::location(2)]] float along : TEXCOORD0;
};

float StrandSpec(float3 t, float3 l, float3 v, float exponent, float shift) {
  float3 t_shifted = normalize(t + shift * float3(0, 1, 0));
  float th = dot(t_shifted, normalize(l + v));
  float sin_th = sqrt(saturate(1.0 - th * th));
  return pow(sin_th, exponent);
}

float4 main(PsIn input) : SV_Target0 {
  float3 base_color = float3(0.35, 0.22, 0.11);  // warm brown
  float3 t = normalize(input.tangent);
  float3 l = normalize(-pc.sun.xyz);
  float3 v = normalize(pc.camera.xyz - input.world_pos);

  // Kajiya-Kay diffuse: sin between tangent and light, wrapped.
  float td = dot(t, l);
  float kd = sqrt(saturate(1.0 - td * td));
  float diffuse = saturate(kd * 0.75 + 0.25);

  float spec1 = StrandSpec(t, l, v, 120.0, -0.06);         // sharp, white-ish
  float spec2 = StrandSpec(t, l, v, 30.0, 0.08);           // broad, tinted
  float root_dark = lerp(0.55, 1.0, input.along);          // fake self-occlusion

  float3 li = pc.sun_color.rgb * pc.sun.w;
  float3 color = base_color * diffuse * root_dark * li * 0.3 +
                 (spec1 * 0.35 + spec2 * 0.2 * base_color * 3.0) * li * 0.25 +
                 base_color * 0.1;  // flat ambient fill
  return float4(color, 1.0);
}
