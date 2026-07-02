// Alpha-tested shadow caster. Opaque materials write depth unconditionally;
// masked materials (foliage, fences) sample the base color and discard below
// the cutoff so their shadows are perforated, not solid blobs. Matches the
// material set (set 1 of the mesh pipeline) bound at set 0 here.
struct MaterialParams {
  float4 base_color_factor;
  float3 emissive_factor;
  float metallic_factor;
  float roughness_factor;
  float alpha_cutoff;
  uint flags;
  float pad;
  float clearcoat;
  float clearcoat_roughness;
  float anisotropy;
  float ior;
  float3 sheen_color;
  float sheen_roughness;
};
[[vk::binding(0, 0)]] ConstantBuffer<MaterialParams> material : register(b0, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D base_color_map : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState base_color_sampler : register(s1, space0);

static const uint kFlagAlphaMask = 1u;

void main([[vk::location(0)]] float2 uv : TEXCOORD0) {
  if ((material.flags & kFlagAlphaMask) != 0u) {
    float a = base_color_map.Sample(base_color_sampler, uv).a * material.base_color_factor.a;
    if (a < material.alpha_cutoff) discard;
  }
}
