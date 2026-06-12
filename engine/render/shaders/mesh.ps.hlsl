// Generated alongside mesh_rt.ps.hlsl from one body; the rt variant adds
// a ray queried shadow toward the sun.

struct FrameGlobals {
  column_major float4x4 view_proj;
  column_major float4x4 prev_view_proj;
  float2 jitter;
  float2 prev_jitter;
  float4 sun_direction;  // xyz travel direction of the light, w intensity
  float4 sun_color;      // rgb color, w ambient
  float4 camera_position;
};
[[vk::binding(0, 0)]] ConstantBuffer<FrameGlobals> frame;

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
[[vk::combinedImageSampler]] [[vk::binding(3, 1)]] Texture2D metallic_roughness_map;
[[vk::combinedImageSampler]] [[vk::binding(3, 1)]] SamplerState metallic_roughness_sampler;
[[vk::combinedImageSampler]] [[vk::binding(4, 1)]] Texture2D emissive_map;
[[vk::combinedImageSampler]] [[vk::binding(4, 1)]] SamplerState emissive_sampler;

static const uint kFlagAlphaMask = 1u;
static const uint kFlagHasNormalMap = 2u;
static const float kPi = 3.14159265359;

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

struct PsOut {
  float4 color : SV_Target0;
  float2 motion : SV_Target1;
};

float3 SurfaceNormal(PsIn input) {
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
  return n;
}

// Cook-Torrance ggx with Schlick fresnel and Smith visibility, single
// directional light plus a flat ambient term.
float3 ShadeSurface(PsIn input, float3 albedo, float3 n, float shadow) {
  float3 v = normalize(frame.camera_position.xyz - input.world_pos);
  if (dot(n, v) < 0.0) n = -n;  // shade double sided geometry from both sides

  // glTF metallic roughness packing: g roughness, b metallic.
  float2 mr = metallic_roughness_map.Sample(metallic_roughness_sampler, input.uv).gb;
  float roughness = clamp(mr.x * material.roughness_factor, 0.045, 1.0);
  float metallic = clamp(mr.y * material.metallic_factor, 0.0, 1.0);

  float3 l = normalize(-frame.sun_direction.xyz);
  float ndl = max(dot(n, l), 0.0);

  float3 f0 = lerp(0.04.xxx, albedo, metallic);
  float3 diffuse_color = albedo * (1.0 - metallic);

  float3 h = normalize(l + v);
  float ndv = max(dot(n, v), 1e-4);
  float ndh = max(dot(n, h), 0.0);
  float vdh = max(dot(v, h), 0.0);
  float a = roughness * roughness;
  float a2 = a * a;
  float denom = ndh * ndh * (a2 - 1.0) + 1.0;
  float distribution = a2 / max(kPi * denom * denom, 1e-6);
  float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
  float vis = (ndv / (ndv * (1.0 - k) + k)) * (ndl / (ndl * (1.0 - k) + k));
  float3 fresnel = f0 + (1.0 - f0) * pow(1.0 - vdh, 5.0);
  float3 specular = distribution * vis * fresnel / max(4.0 * ndv * ndl, 1e-4);

  float3 sun = frame.sun_color.rgb * frame.sun_direction.w;
  float3 lit = (diffuse_color / kPi + specular) * sun * ndl * shadow;
  float3 ambient = albedo * frame.sun_color.w;
  float3 emissive = emissive_map.Sample(emissive_sampler, input.uv).rgb * material.emissive_factor;
  return lit + ambient + emissive;
}

float SunShadow(PsIn input, float3 n) { return 1.0; }

PsOut main(PsIn input) {
  float4 base = base_color_map.Sample(base_color_sampler, input.uv) *
                material.base_color_factor * input.color;
  if ((material.flags & kFlagAlphaMask) != 0u && base.a < material.alpha_cutoff) discard;

  float3 n = SurfaceNormal(input);
  float shadow = SunShadow(input, n);

  PsOut output;
  output.color = float4(ShadeSurface(input, base.rgb, n, shadow), 1.0);
  // Uv offset from this pixel to where the surface was last frame.
  float2 curr = input.curr_clip.xy / input.curr_clip.w;
  float2 prev = input.prev_clip.xy / input.prev_clip.w;
  output.motion = (prev - curr) * 0.5;
  return output;
}
