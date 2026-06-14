// Generated alongside mesh_rt.ps.hlsl from one body; the rt variant adds
// a ray queried shadow toward the sun.

struct FrameGlobals {
  column_major float4x4 view_proj;
  column_major float4x4 prev_view_proj;
  column_major float4x4 inv_view_proj;
  float2 jitter;
  float2 prev_jitter;
  float4 sun_direction;  // xyz travel direction of the light, w intensity
  float4 sun_color;      // rgb color, w flat ambient when ibl is off
  float4 camera_position;  // xyz eye, w ibl intensity
  float4 misc;             // x,y render size, z sun angular radius, w frame index
  uint flags;
  float time;
  uint debug_view;  // render::DebugView, isolates a shading channel
  float reflection_cutoff;
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
  float clearcoat;
  float clearcoat_roughness;
  float anisotropy;
  float ior;
  float3 sheen_color;
  float sheen_roughness;
  float3 subsurface_color;
  float subsurface;
  float iridescence;
  float iridescence_thickness;
  float2 irid_pad;
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

[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] TextureCube irradiance_cube;
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] SamplerState irradiance_sampler;
[[vk::combinedImageSampler]] [[vk::binding(1, 2)]] TextureCube prefiltered_cube;
[[vk::combinedImageSampler]] [[vk::binding(1, 2)]] SamplerState prefiltered_sampler;
[[vk::combinedImageSampler]] [[vk::binding(2, 2)]] Texture2D brdf_lut;
[[vk::combinedImageSampler]] [[vk::binding(2, 2)]] SamplerState brdf_lut_sampler;
[[vk::combinedImageSampler]] [[vk::binding(3, 2)]] Texture2D ao_map;
[[vk::combinedImageSampler]] [[vk::binding(3, 2)]] SamplerState ao_sampler;
[[vk::combinedImageSampler]] [[vk::binding(4, 2)]] Texture2DArray ddgi_irradiance;
[[vk::combinedImageSampler]] [[vk::binding(4, 2)]] SamplerState ddgi_irradiance_sampler;
[[vk::combinedImageSampler]] [[vk::binding(5, 2)]] Texture2DArray ddgi_distance;
[[vk::combinedImageSampler]] [[vk::binding(5, 2)]] SamplerState ddgi_distance_sampler;

struct DdgiVolume {
  float4 origin;          // xyz grid origin, w probe spacing
  uint4 counts;           // xyz probe counts, w irradiance texel resolution
  float4 params;          // x distance texel resolution, y hysteresis,
                          // z max ray distance, w energy scale
};
[[vk::binding(6, 2)]] ConstantBuffer<DdgiVolume> ddgi;

[[vk::combinedImageSampler]] [[vk::binding(7, 2)]] Texture2D shadow_atlas;
[[vk::combinedImageSampler]] [[vk::binding(7, 2)]] SamplerComparisonState shadow_sampler;

struct CascadeData {
  column_major float4x4 light_view_proj[4];
  float4 p0;  // x cascade count, y depth bias, z 1/count, w atlas inset
  float4 p1;  // x cascade-local texel, y unused, z normal bias, w unused
};
[[vk::binding(8, 2)]] ConstantBuffer<CascadeData> cascades;

static const uint kFlagAlphaMask = 1u;
static const uint kFlagHasNormalMap = 2u;
static const uint kFrameIbl = 1u;
static const uint kFrameAoValid = 2u;
static const uint kFrameDdgi = 4u;
static const uint kFrameShadowMap = 64u;
static const float kPi = 3.14159265359;
static const float kPrefilterMips = 6.0;

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

// Octahedral mapping of a unit direction onto a probe texel footprint.
float2 OctEncode(float3 d) {
  d /= (abs(d.x) + abs(d.y) + abs(d.z));
  float2 o = d.xz;
  if (d.y < 0.0) o = (1.0 - abs(d.zx)) * float2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
  return o;
}

float2 ProbeAtlasUv(uint3 probe, float3 dir, float texels, float2 atlas_size) {
  float2 oct = OctEncode(dir) * 0.5 + 0.5;
  float2 base = float2(probe.x + probe.z * ddgi.counts.x, probe.y) * (texels + 2.0) + 1.0;
  return (base + oct * texels) / atlas_size;
}

// Trilinear probe blend with chebyshev visibility, the DDGI estimator.
float3 SampleDdgi(float3 world_pos, float3 n, float3 v) {
  float spacing = ddgi.origin.w;
  float3 local = (world_pos - ddgi.origin.xyz) / spacing;
  if (any(local < 0.0) || any(local > float3(ddgi.counts.xyz - 1))) return 0.0.xxx;

  // Surface bias along normal and view keeps samples out of the wall.
  float3 biased = world_pos + (n * 0.2 + v * 0.8) * spacing * 0.25;
  float3 local_biased = clamp((biased - ddgi.origin.xyz) / spacing,
                              0.0.xxx, float3(ddgi.counts.xyz) - 1.001);
  uint3 base_probe = (uint3)local_biased;
  float3 alpha = frac(local_biased);

  float irr_texels = (float)ddgi.counts.w;
  float dist_texels = ddgi.params.x;
  float2 irr_atlas = float2((ddgi.counts.w + 2) * ddgi.counts.x * ddgi.counts.z,
                            (ddgi.counts.w + 2) * ddgi.counts.y);
  float2 dist_atlas = float2((ddgi.params.x + 2.0) * ddgi.counts.x * ddgi.counts.z,
                             (ddgi.params.x + 2.0) * ddgi.counts.y);

  float3 sum = 0.0.xxx;
  float weight_sum = 0.0;
  [unroll]
  for (uint i = 0; i < 8; ++i) {
    uint3 offset = uint3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
    uint3 probe = min(base_probe + offset, ddgi.counts.xyz - 1);
    float3 probe_pos = ddgi.origin.xyz + float3(probe) * spacing;

    float3 tri = lerp(1.0 - alpha, alpha, float3(offset));
    float weight = tri.x * tri.y * tri.z;

    // Backface: probes behind the surface contribute nothing.
    float3 to_probe = normalize(probe_pos - world_pos);
    float facing = (dot(to_probe, n) + 1.0) * 0.5;
    weight *= facing * facing + 0.2;

    // Chebyshev visibility against the probe's depth map.
    float3 from_probe = biased - probe_pos;
    float dist = length(from_probe);
    float2 moments = ddgi_distance
        .SampleLevel(ddgi_distance_sampler,
                     float3(ProbeAtlasUv(probe, from_probe / max(dist, 1e-4), dist_texels,
                                         dist_atlas), 0.0), 0.0).rg;
    if (dist > moments.x) {
      float variance = abs(moments.y - moments.x * moments.x);
      float diff = dist - moments.x;
      float visibility = variance / (variance + diff * diff);
      weight *= max(visibility * visibility * visibility, 0.05);
    }

    weight = max(weight, 1e-4);
    float3 irr = ddgi_irradiance
        .SampleLevel(ddgi_irradiance_sampler,
                     float3(ProbeAtlasUv(probe, n, irr_texels, irr_atlas), 0.0), 0.0).rgb;
    sum += sqrt(irr) * weight;  // blend in perceptual space, square after
    weight_sum += weight;
  }
  float3 mean = sum / max(weight_sum, 1e-4);
  return mean * mean * ddgi.params.w;
}

// --- BRDF lobes shared by the base, clearcoat, sheen and anisotropy paths ---
float D_GGX(float ndh, float a) {
  float a2 = a * a;
  float d = ndh * ndh * (a2 - 1.0) + 1.0;
  return a2 / max(kPi * d * d, 1e-7);
}
float V_SmithGGXCorrelated(float ndv, float ndl, float a) {
  float a2 = a * a;
  float gv = ndl * sqrt(ndv * ndv * (1.0 - a2) + a2);
  float gl = ndv * sqrt(ndl * ndl * (1.0 - a2) + a2);
  return 0.5 / max(gv + gl, 1e-5);
}
float D_GGXAniso(float ndh, float tdh, float bdh, float ax, float ay) {
  float d = tdh * tdh / (ax * ax) + bdh * bdh / (ay * ay) + ndh * ndh;
  return 1.0 / max(kPi * ax * ay * d * d, 1e-7);
}
float V_GGXAniso(float ndv, float ndl, float tdv, float bdv, float tdl, float bdl, float ax,
                 float ay) {
  float gv = ndl * length(float3(ax * tdv, ay * bdv, ndv));
  float gl = ndv * length(float3(ax * tdl, ay * bdl, ndl));
  return 0.5 / max(gv + gl, 1e-5);
}
// Charlie sheen distribution + Ashikhmin visibility (glTF KHR_materials_sheen).
float D_Charlie(float ndh, float roughness) {
  float a = max(roughness, 0.07);
  float inv = 1.0 / a;
  float sin2 = max(1.0 - ndh * ndh, 0.0);
  return (2.0 + inv) * pow(sin2, inv * 0.5) / (2.0 * kPi);
}
float V_Ashikhmin(float ndv, float ndl) {
  return clamp(1.0 / (4.0 * (ndl + ndv - ndl * ndv)), 0.0, 1.0);
}
// Thin-film interference: an iridescent rgb from the optical path difference at
// the view angle, blended into f0 (soap bubble / oil slick look).
float3 ThinFilm(float ndv, float thickness_nm, float film_ior) {
  float opd = 2.0 * film_ior * thickness_nm * ndv;  // path difference, nm
  float3 wl = float3(612.0, 549.0, 465.0);          // r,g,b wavelengths, nm
  float3 phase = 6.2831853 * opd / wl;
  return 0.5 + 0.5 * cos(phase);
}

// Cook-Torrance ggx with Schlick fresnel and Smith visibility for the sun,
// split-sum ibl with Fdez-Aguera multi-scatter for ambient. Optional clearcoat,
// sheen and anisotropy lobes layer on top.
float3 ShadeSurface(PsIn input, float3 albedo, float3 n, float shadow) {
  float3 v = normalize(frame.camera_position.xyz - input.world_pos);
  if (dot(n, v) < 0.0) n = -n;  // shade double sided geometry from both sides

  // glTF metallic roughness packing: g roughness, b metallic.
  float2 mr = metallic_roughness_map.Sample(metallic_roughness_sampler, input.uv).gb;
  float roughness = clamp(mr.x * material.roughness_factor, 0.045, 1.0);
  float metallic = clamp(mr.y * material.metallic_factor, 0.0, 1.0);

  float3 l = normalize(-frame.sun_direction.xyz);
  float ndl = max(dot(n, l), 0.0);

  // Dielectric f0 from the ior (1.5 reproduces the classic 0.04).
  float dielectric_f0 = pow((material.ior - 1.0) / (material.ior + 1.0), 2.0);
  float3 f0 = lerp(dielectric_f0.xxx, albedo, metallic);
  float3 diffuse_color = albedo * (1.0 - metallic);

  float3 h = normalize(l + v);
  float ndv = max(dot(n, v), 1e-4);
  float ndh = max(dot(n, h), 0.0);
  float vdh = max(dot(v, h), 0.0);
  float a = roughness * roughness;
  if (material.iridescence > 0.001) {
    f0 = lerp(f0, ThinFilm(ndv, material.iridescence_thickness, 1.3), material.iridescence);
  }
  float3 fresnel = f0 + (1.0 - f0) * pow(1.0 - vdh, 5.0);

  float3 specular;
  if (abs(material.anisotropy) > 0.001) {
    // Anisotropic ggx along the surface tangent (brushed-metal streaks).
    float3 tan_w = normalize(input.tangent.xyz - n * dot(input.tangent.xyz, n));
    float3 bitan_w = cross(n, tan_w) * input.tangent.w;
    float aniso = clamp(material.anisotropy, -1.0, 1.0);
    float ax = max(a * (1.0 + aniso), 1e-3);
    float ay = max(a * (1.0 - aniso), 1e-3);
    float d_aniso = D_GGXAniso(ndh, dot(tan_w, h), dot(bitan_w, h), ax, ay);
    float v_aniso = V_GGXAniso(ndv, ndl, dot(tan_w, v), dot(bitan_w, v), dot(tan_w, l),
                               dot(bitan_w, l), ax, ay);
    specular = d_aniso * v_aniso * fresnel;
  } else {
    specular = D_GGX(ndh, a) * V_SmithGGXCorrelated(ndv, ndl, a) * fresnel;
  }

  float3 direct = diffuse_color / kPi + specular;

  // Sheen: a retroreflective lobe for cloth, added over the base.
  if (dot(material.sheen_color, 1.0) > 0.001) {
    direct += material.sheen_color * D_Charlie(ndh, material.sheen_roughness) *
              V_Ashikhmin(ndv, ndl);
  }

  // Clearcoat: a smooth ggx lobe over a 1.5-ior coat that dims the base by its
  // own fresnel reflectance.
  float coat_fresnel_v = 0.04 + 0.96 * pow(1.0 - ndv, 5.0);
  if (material.clearcoat > 0.001) {
    float cc_a = max(material.clearcoat_roughness * material.clearcoat_roughness, 1e-3);
    float cc_f = (0.04 + 0.96 * pow(1.0 - vdh, 5.0)) * material.clearcoat;
    direct = direct * (1.0 - cc_f) + D_GGX(ndh, cc_a) * V_SmithGGXCorrelated(ndv, ndl, cc_a) * cc_f;
  }

  float3 sun = frame.sun_color.rgb * frame.sun_direction.w;
  float3 lit = direct * sun * ndl * shadow;

  // Subsurface scattering: a wrapped front term softens the terminator and a
  // view-aligned back term glows where light transmits through thin geometry.
  if (material.subsurface > 0.001) {
    float3 sss_h = normalize(l + n * 0.2);
    float back = pow(saturate(dot(v, -sss_h)), 2.0);
    float wrap = saturate((dot(n, l) + 0.4) / 1.4);
    lit += material.subsurface_color * material.subsurface * (back + 0.4 * wrap) * sun * shadow;
  }

  float ao = 1.0;
  if ((frame.flags & kFrameAoValid) != 0u) {
    ao = ao_map.Sample(ao_sampler, input.sv_position.xy / frame.misc.xy).r;
  }

  float3 ambient;
  if ((frame.flags & kFrameIbl) != 0u) {
    float2 f_ab = brdf_lut.Sample(brdf_lut_sampler, float2(ndv, roughness)).rg;
    float3 r = reflect(-v, n);
    float3 radiance =
        prefiltered_cube.SampleLevel(prefiltered_sampler, r, roughness * (kPrefilterMips - 1.0)).rgb;
    float3 irradiance = irradiance_cube.Sample(irradiance_sampler, n).rgb;
    if ((frame.flags & kFrameDdgi) != 0u) {
      irradiance += SampleDdgi(input.world_pos, n, v);
    }
    // Fdez-Aguera energy compensation: single scatter split-sum plus a
    // multiple scattering term so rough metals stop losing energy.
    float3 fss_ess = f0 * f_ab.x + f_ab.y;
    float ems = 1.0 - (f_ab.x + f_ab.y);
    float3 f_avg = f0 + (1.0 - f0) / 21.0;
    float3 fms_ems = ems * fss_ess * f_avg / (1.0 - f_avg * ems);
    float3 k_d = diffuse_color * (1.0 - fss_ess - fms_ems);
    ambient = (fss_ess * radiance + (fms_ems + k_d) * irradiance) * frame.camera_position.w;
  } else {
    ambient = albedo * frame.sun_color.w;
  }
  ambient *= ao;

  // Clearcoat reflects the environment through its smooth coat as well.
  if (material.clearcoat > 0.001 && (frame.flags & kFrameIbl) != 0u) {
    float cc_r = clamp(material.clearcoat_roughness, 0.045, 1.0);
    float3 coat_refl = prefiltered_cube
        .SampleLevel(prefiltered_sampler, reflect(-v, n), cc_r * (kPrefilterMips - 1.0)).rgb;
    float cc_f = coat_fresnel_v * material.clearcoat;
    ambient = ambient * (1.0 - cc_f) + coat_refl * cc_f * frame.camera_position.w;
  }

  float3 emissive = emissive_map.Sample(emissive_sampler, input.uv).rgb * material.emissive_factor;

  // Debug channels isolate one shading input so it can be eyeballed. They share
  // the lit path's exact data, so they verify those inputs, not a separate copy.
  switch (frame.debug_view) {
    case 1: return albedo;
    case 2: return n * 0.5 + 0.5;
    case 3: return roughness.xxx;
    case 4: return metallic.xxx;
    case 5: return ao.xxx;
    case 6: return ambient;     // indirect (ibl + ddgi), ao applied
    case 7: return lit;         // direct sun, shadowed
    case 8: return emissive;
  }
  return lit + ambient + emissive;
}

// Cascaded shadow map lookup: pick the tightest cascade that contains the
// (normal-biased) point, then 3x3 pcf through the comparison sampler. Returns
// 1 (lit) when shadow maps are off or the point falls beyond the last cascade.
float SunShadow(PsIn input, float3 n) {
  if ((frame.flags & kFrameShadowMap) == 0u) return 1.0;
  uint count = (uint)cascades.p0.x;
  float bias = cascades.p0.y;
  float inv_count = cascades.p0.z;
  float inset = cascades.p0.w;
  float texel = cascades.p1.x;
  float3 biased = input.world_pos + n * cascades.p1.z;

  for (uint i = 0; i < count; ++i) {
    float4 clip = mul(cascades.light_view_proj[i], float4(biased, 1.0));
    float3 ndc = clip.xyz / clip.w;
    float2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < inset || uv.x > 1.0 - inset || uv.y < inset || uv.y > 1.0 - inset) continue;
    if (ndc.z < 0.0 || ndc.z > 1.0) continue;

    float ref = ndc.z - bias;
    float sum = 0.0;
    [unroll]
    for (int oy = -1; oy <= 1; ++oy) {
      [unroll]
      for (int ox = -1; ox <= 1; ++ox) {
        float2 luv = clamp(uv + float2(ox, oy) * texel, inset, 1.0 - inset);
        float2 atlas_uv = float2((float(i) + luv.x) * inv_count, luv.y);
        sum += shadow_atlas.SampleCmpLevelZero(shadow_sampler, atlas_uv, ref);
      }
    }
    return sum / 9.0;
  }
  return 1.0;
}

PsOut main(PsIn input) {
  float4 base = base_color_map.Sample(base_color_sampler, input.uv) *
                material.base_color_factor * input.color;
  if ((material.flags & kFlagAlphaMask) != 0u && base.a < material.alpha_cutoff) discard;

  float3 n = SurfaceNormal(input);
  float shadow = SunShadow(input, n);

  PsOut output;
  // Alpha carries through for the blend pass; opaque targets ignore it.
  output.color = float4(ShadeSurface(input, base.rgb, n, shadow), base.a);
  // Uv offset from this pixel to where the surface was last frame.
  float2 curr = input.curr_clip.xy / input.curr_clip.w;
  float2 prev = input.prev_clip.xy / input.prev_clip.w;
  output.motion = (prev - curr) * 0.5;
  return output;
}
