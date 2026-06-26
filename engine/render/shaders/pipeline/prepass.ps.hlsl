// Depth, octahedral normal and motion prepass. Feeds rtao and the temporal
// passes, and lets the main pass run with depth EQUAL and no overdraw
// shading. The main pass masks off its motion output; only the sky pass
// adds motion on top of this.

struct MaterialParams {
  float4 base_color_factor;
  float3 emissive_factor;
  float metallic_factor;
  float roughness_factor;
  float alpha_cutoff;
  uint flags;
  float pad;
};
[[vk::binding(0, 1)]] ConstantBuffer<MaterialParams> material;

[[vk::combinedImageSampler]] [[vk::binding(1, 1)]] Texture2D base_color_map;
[[vk::combinedImageSampler]] [[vk::binding(1, 1)]] SamplerState base_color_sampler;
[[vk::combinedImageSampler]] [[vk::binding(2, 1)]] Texture2D normal_map;
[[vk::combinedImageSampler]] [[vk::binding(2, 1)]] SamplerState normal_sampler;

static const uint kFlagAlphaMask = 1u;
static const uint kFlagHasNormalMap = 2u;

struct PsIn {
  float4 sv_position : SV_Position;
  [[vk::location(0)]] float3 normal : NORMAL;
  [[vk::location(1)]] float4 curr_clip : TEXCOORD1;
  [[vk::location(2)]] float4 prev_clip : TEXCOORD2;
  [[vk::location(3)]] float3 world_pos : TEXCOORD3;
  [[vk::location(4)]] float4 tangent : TANGENT;
  [[vk::location(5)]] float2 uv : TEXCOORD0;
  [[vk::location(6)]] float4 color : COLOR0;
};

float2 OctEncode(float3 d) {
  d /= (abs(d.x) + abs(d.y) + abs(d.z));
  float2 o = d.xz;
  if (d.y < 0.0) o = (1.0 - abs(d.zx)) * float2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
  return o;
}

struct PsOut {
  float2 normal : SV_Target0;
  float2 motion : SV_Target1;
  // Raw reversed-z exported as color: downstream passes sample this instead
  // of the depth attachment, which then never leaves attachment layout.
  float depth : SV_Target2;
};

PsOut main(PsIn input) {
  if ((material.flags & kFlagAlphaMask) != 0u) {
    float4 base = base_color_map.Sample(base_color_sampler, input.uv) *
                  material.base_color_factor * input.color;
    if (base.a < material.alpha_cutoff) discard;
  }
  float3 n = normalize(input.normal);
  if ((material.flags & kFlagHasNormalMap) != 0u) {
    float3 t = input.tangent.xyz - n * dot(input.tangent.xyz, n);
    if (dot(t, t) > 1e-8) {
      t = normalize(t);
      float3 b = cross(n, t) * input.tangent.w;
      float3 tn = normal_map.Sample(normal_sampler, input.uv).xyz * 2.0 - 1.0;
      n = normalize(tn.x * t + tn.y * b + tn.z * n);
    }
  }
  PsOut output;
  output.normal = OctEncode(n);
  float2 curr = input.curr_clip.xy / input.curr_clip.w;
  float2 prev = input.prev_clip.xy / input.prev_clip.w;
  output.motion = (prev - curr) * 0.5;
  output.depth = input.sv_position.z;
  return output;
}
