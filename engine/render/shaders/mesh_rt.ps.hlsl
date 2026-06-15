// mesh.ps.hlsl with ray queried sun shadows, cone-jittered for soft
// penumbras that the temporal passes integrate.

[[vk::binding(1, 0)]] RaytracingAccelerationStructure tlas;

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
  float reflection_cutoff;  // roughness above which reflections fall back to ibl
  uint ao_ray_count;        // rt ao rays/pixel this frame (0 when ao is screen-space)
  uint light_count;         // dynamic point lights in the light buffer
};
[[vk::binding(0, 0)]] ConstantBuffer<FrameGlobals> frame;

struct PointLight {
  float4 pos_radius;       // xyz position, w influence radius
  float4 color_intensity;  // rgb color, w intensity
};
[[vk::binding(11, 2)]] StructuredBuffer<PointLight> point_lights;

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
  float transmission;
  float irid_pad;
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
[[vk::combinedImageSampler]] [[vk::binding(9, 2)]] Texture2D opaque_scene;  // for transmission
[[vk::combinedImageSampler]] [[vk::binding(9, 2)]] SamplerState opaque_scene_sampler;
[[vk::combinedImageSampler]] [[vk::binding(10, 2)]] Texture2D sun_shadow_map;  // SIGMA-denoised
[[vk::combinedImageSampler]] [[vk::binding(10, 2)]] SamplerState sun_shadow_sampler;

// Scene tables for reflection hit shading (set 3), matching water.ps.hlsl.
struct MeshRecord {
  uint64_t vertex_address;
  uint64_t index_address;
  uint geometry_offset;
  uint pad0;
  uint pad1;
  uint pad2;
};
struct GeometryRecord {
  uint index_offset;
  uint material_index;
};
struct MaterialRecord {
  float4 base_color_factor;
  float3 emissive;
  uint base_color_texture;
};
[[vk::binding(0, 3)]] StructuredBuffer<MeshRecord> mesh_records;
[[vk::binding(1, 3)]] StructuredBuffer<GeometryRecord> geometry_records;
[[vk::binding(2, 3)]] StructuredBuffer<MaterialRecord> material_records;
[[vk::binding(3, 3)]] Texture2D bindless_textures[];
[[vk::binding(4, 3)]] SamplerState bindless_sampler;

static const uint kFlagAlphaMask = 1u;
static const uint kFlagHasNormalMap = 2u;
static const uint kFrameIbl = 1u;
static const uint kFrameAoValid = 2u;
static const uint kFrameDdgi = 4u;
static const uint kFrameReflections = 16u;
static const uint kFrameRtShadows = 32u;
static const uint kFrameSigmaShadow = 128u;  // sample the denoised sun shadow
static const float kPi = 3.14159265359;
static const float kPrefilterMips = 6.0;
static const uint kVertexStride = 52;
static const uint kNormalOffset = 12;
static const uint kUvOffset = 40;

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

// Nearest-probe diffuse gi for a reflection hit (cheaper than the full blend).
float3 SampleDdgiNearest(float3 world_pos, float3 n) {
  if ((frame.flags & kFrameDdgi) == 0u) return 0.0.xxx;
  float3 local = (world_pos - ddgi.origin.xyz) / ddgi.origin.w;
  if (any(local < 0.0) || any(local > float3(ddgi.counts.xyz - 1))) return 0.0.xxx;
  uint3 probe = (uint3)round(local);
  float texels = (float)ddgi.counts.w;
  float2 atlas = float2((ddgi.counts.w + 2) * ddgi.counts.x * ddgi.counts.z,
                        (ddgi.counts.w + 2) * ddgi.counts.y);
  float3 irr = ddgi_irradiance
      .SampleLevel(ddgi_irradiance_sampler,
                   float3(ProbeAtlasUv(probe, n, texels, atlas), 0.0), 0.0).rgb;
  return irr * irr * ddgi.params.w;
}

// One reflection bounce through the scene tables: sun (no secondary shadow) +
// nearest-probe gi + emissive at the hit, prefiltered sky on miss.
float3 TraceReflection(float3 origin, float3 dir) {
  RayDesc ray;
  ray.Origin = origin;
  ray.TMin = 0.02;
  ray.Direction = dir;
  ray.TMax = 200.0;
  RayQuery<RAY_FLAG_FORCE_OPAQUE> rq;
  rq.TraceRayInline(tlas, RAY_FLAG_NONE, 0xff, ray);
  rq.Proceed();
  if (rq.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
    return min(prefiltered_cube.SampleLevel(prefiltered_sampler, dir, 1.0).rgb, 8.0.xxx);
  }

  float3 hit_pos = origin + dir * rq.CommittedRayT();
  MeshRecord mesh = mesh_records[NonUniformResourceIndex(rq.CommittedInstanceID())];
  GeometryRecord geometry = geometry_records[mesh.geometry_offset + rq.CommittedGeometryIndex()];
  uint64_t index_base =
      mesh.index_address + (geometry.index_offset + rq.CommittedPrimitiveIndex() * 3) * 4;
  uint3 tri;
  tri.x = vk::RawBufferLoad<uint>(index_base);
  tri.y = vk::RawBufferLoad<uint>(index_base + 4);
  tri.z = vk::RawBufferLoad<uint>(index_base + 8);
  float2 bary = rq.CommittedTriangleBarycentrics();
  float3 w = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
  float3 n_local = 0.0.xxx;
  float2 uv = 0.0.xx;
  [unroll]
  for (uint corner = 0; corner < 3; ++corner) {
    uint64_t vertex = mesh.vertex_address + tri[corner] * kVertexStride;
    n_local += vk::RawBufferLoad<float3>(vertex + kNormalOffset, 4) * w[corner];
    uv += vk::RawBufferLoad<float2>(vertex + kUvOffset, 4) * w[corner];
  }
  float3x4 to_world = rq.CommittedObjectToWorld3x4();
  float3 hit_n = normalize(mul((float3x3)to_world, n_local));
  if (dot(hit_n, dir) > 0.0) hit_n = -hit_n;

  MaterialRecord hit_material =
      material_records[NonUniformResourceIndex(geometry.material_index)];
  float3 albedo = hit_material.base_color_factor.rgb;
  if (hit_material.base_color_texture != 0xffffffffu) {
    albedo *= bindless_textures[NonUniformResourceIndex(hit_material.base_color_texture)]
                  .SampleLevel(bindless_sampler, uv, 2.0).rgb;
  }
  float3 to_sun = normalize(-frame.sun_direction.xyz);
  float3 sun = frame.sun_color.rgb * frame.sun_direction.w;
  float ndl = max(dot(hit_n, to_sun), 0.0);
  return albedo / kPi * sun * ndl + albedo * SampleDdgiNearest(hit_pos, hit_n) +
         hit_material.emissive;
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

  // Dynamic point lights: ggx + diffuse with smooth inverse-square-ish falloff
  // clamped to the influence radius. light_hits drives the complexity view.
  uint light_hits = 0;
  for (uint li = 0; li < frame.light_count; ++li) {
    PointLight pl = point_lights[li];
    float3 to_l = pl.pos_radius.xyz - input.world_pos;
    float dist2 = dot(to_l, to_l);
    float lr = pl.pos_radius.w;
    if (dist2 >= lr * lr) continue;
    float dist = sqrt(max(dist2, 1e-8));
    float3 pl_l = to_l / dist;
    float pndl = max(dot(n, pl_l), 0.0);
    if (pndl <= 0.0) continue;
    ++light_hits;
    float falloff = saturate(1.0 - dist2 / (lr * lr));
    falloff *= falloff;
    float3 pl_h = normalize(pl_l + v);
    float pndh = max(dot(n, pl_h), 0.0);
    float pvdh = max(dot(v, pl_h), 0.0);
    float3 pf = f0 + (1.0 - f0) * pow(1.0 - pvdh, 5.0);
    float3 pspec = D_GGX(pndh, a) * V_SmithGGXCorrelated(ndv, pndl, a) * pf;
    float3 pdiff = diffuse_color * (1.0 / kPi) * (1.0 - pf);
    lit += (pdiff + pspec) * pl.color_intensity.rgb * pl.color_intensity.w * falloff * pndl;
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
    // Hybrid specular: trace the reflection for smooth surfaces, fade to the
    // prefiltered probe as roughness climbs to the cutoff (which gets blurry
    // enough that the cube is indistinguishable and far cheaper).
    if ((frame.flags & kFrameReflections) != 0u && roughness < frame.reflection_cutoff) {
      float blend = saturate(roughness / max(frame.reflection_cutoff, 1e-3));
      float3 traced = TraceReflection(input.world_pos + n * 0.02, r);
      radiance = lerp(traced, radiance, blend);
    }
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
    case 9: return TraceReflection(input.world_pos + n * 0.02, reflect(-v, n));  // raw reflection
    case 14: {  // ray-count heatmap: shadow + ao + reflection rays this pixel casts
      float rays = float(frame.ao_ray_count);
      if ((frame.flags & kFrameRtShadows) != 0u && dot(n, l) > 0.0) rays += 1.0;
      if ((frame.flags & kFrameReflections) != 0u && roughness < frame.reflection_cutoff)
        rays += 1.0;
      if (rays <= 0.0) return float3(0.02, 0.02, 0.02);
      float t = saturate(rays / 8.0);  // blue (cheap) -> red (~8 rays)
      return saturate(float3(1.5 - abs(4.0 * t - 3.0), 1.5 - abs(4.0 * t - 2.0),
                             1.5 - abs(4.0 * t - 1.0)));
    }
    case 15: {  // light-complexity heatmap: point lights affecting this pixel
      if (light_hits == 0u) return float3(0.02, 0.02, 0.02);
      float t = saturate(float(light_hits) / 6.0);
      return saturate(float3(1.5 - abs(4.0 * t - 3.0), 1.5 - abs(4.0 * t - 2.0),
                             1.5 - abs(4.0 * t - 1.0)));
    }
  }
  return lit + ambient + emissive;
}

// Interleaved gradient noise, decorrelated across frames by the golden
// ratio so temporal accumulation averages the penumbra.
float ShadowNoise(float2 pixel, float offset) {
  float ign = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
  return frac(ign + offset * 0.61803398875);
}

float SunShadow(PsIn input, float3 n) {
  if ((frame.flags & kFrameRtShadows) == 0u) return 1.0;  // reflections-only rt frame
  float3 l = normalize(-frame.sun_direction.xyz);
  if (dot(n, l) <= 0.0) return 1.0;  // ndl already zeroes the contribution

  // When SIGMA denoised the sun shadow this frame, sample the screen-space
  // result instead of tracing inline (R8 stores sqrt(visibility), decode by
  // squaring). The dedicated trace + denoiser gives a cleaner penumbra than the
  // 1-spp inline cone the temporal pass would otherwise have to integrate.
  if ((frame.flags & kFrameSigmaShadow) != 0u) {
    float2 screen = input.sv_position.xy / frame.misc.xy;
    float s = sun_shadow_map.SampleLevel(sun_shadow_sampler, screen, 0.0).r;
    return s * s;
  }

  // Jitter the ray inside the sun's angular radius for soft penumbras; the
  // temporal pass integrates the cone.
  float radius = frame.misc.z;
  if (radius > 0.0) {
    float3 up = abs(l.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 t1 = normalize(cross(up, l));
    float3 t2 = cross(l, t1);
    float u1 = ShadowNoise(input.sv_position.xy, frame.misc.w);
    float u2 = ShadowNoise(input.sv_position.yx + 17.0, frame.misc.w * 1.7);
    float angle = 6.2831853 * u1;
    float r = sqrt(u2) * tan(radius);
    l = normalize(l + t1 * (cos(angle) * r) + t2 * (sin(angle) * r));
  }

  RayDesc ray;
  ray.Origin = input.world_pos + n * 0.01;
  ray.TMin = 0.001;
  ray.Direction = l;
  ray.TMax = 1000.0;
  RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> rq;
  rq.TraceRayInline(tlas, RAY_FLAG_NONE, 0xff, ray);
  rq.Proceed();
  return rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
}

// Transmission: refract the opaque scene behind the surface (screen-space),
// tint it by the base color and add a fresnel rim reflection, for glass.
float3 ApplyTransmission(float3 shaded, PsIn input, float3 n, float3 base_color) {
  float3 v = normalize(frame.camera_position.xyz - input.world_pos);
  float ndv = max(dot(n, v), 1e-4);
  float2 screen = input.sv_position.xy / frame.misc.xy;
  float2 refr = n.xy * 0.06 * material.transmission;
  float3 bg = opaque_scene.SampleLevel(opaque_scene_sampler, saturate(screen + refr), 0.0).rgb;
  float3 transmitted = bg * lerp(1.0.xxx, base_color, 0.6);
  float fres = 0.04 + 0.96 * pow(saturate(1.0 - ndv), 5.0);
  float3 refl = transmitted;
  if ((frame.flags & kFrameIbl) != 0u) {
    refl = prefiltered_cube.SampleLevel(prefiltered_sampler, reflect(-v, n), 0.0).rgb *
           frame.camera_position.w;
  }
  return lerp(shaded, lerp(transmitted, refl, fres), material.transmission);
}

PsOut main(PsIn input) {
  float4 base = base_color_map.Sample(base_color_sampler, input.uv) *
                material.base_color_factor * input.color;
  if ((material.flags & kFlagAlphaMask) != 0u && base.a < material.alpha_cutoff) discard;

  float3 n = SurfaceNormal(input);
  float shadow = SunShadow(input, n);

  float3 shaded = ShadeSurface(input, base.rgb, n, shadow);
  float alpha = base.a;
  if (material.transmission > 0.001 && frame.debug_view == 0u) {
    shaded = ApplyTransmission(shaded, input, n, base.rgb);
    alpha = 1.0;
  }

  PsOut output;
  // Alpha carries through for the blend pass; opaque targets ignore it.
  output.color = float4(shaded, alpha);
  // Uv offset from this pixel to where the surface was last frame.
  float2 curr = input.curr_clip.xy / input.curr_clip.w;
  float2 prev = input.prev_clip.xy / input.prev_clip.w;
  output.motion = (prev - curr) * 0.5;
  return output;
}
