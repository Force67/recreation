// mesh.ps.hlsl with ray queried sun shadows, cone-jittered for soft
// penumbras that the temporal passes integrate.

[[vk::binding(1, 0)]] RaytracingAccelerationStructure tlas : register(t1, space0);

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
  float2 pad_wind;
  float4 wind;
  float4 cluster_params;  // x slice scale, y slice bias, zw tile size px
};
[[vk::binding(0, 0)]] ConstantBuffer<FrameGlobals> frame : register(b0, space0);

struct PointLight {
  float4 pos_radius;       // xyz position, w influence radius
  float4 color_intensity;  // rgb color, w intensity
  float4 direction_type;   // xyz emit direction, w type (0 point 1 spot 2 sphere 3 rect)
  float4 params;           // spot: cos inner/outer; sphere: source radius; rect: half extents
};
[[vk::binding(11, 2)]] StructuredBuffer<PointLight> point_lights : register(t11, space2);

// Froxel clustering (light_cluster.cs writes these; mirrors mesh_pipeline.h).
[[vk::binding(13, 2)]] StructuredBuffer<uint> cluster_counts : register(t13, space2);
[[vk::binding(14, 2)]] StructuredBuffer<uint> cluster_indices : register(t14, space2);
static const uint kClusterTilesX = 16;
static const uint kClusterTilesY = 9;
static const uint kClusterSlices = 24;
static const uint kMaxLightsPerCluster = 32;

uint ClusterOf(float2 sv_xy, float view_z) {
  uint tx = min(uint(sv_xy.x / frame.cluster_params.z), kClusterTilesX - 1u);
  uint ty = min(uint(sv_xy.y / frame.cluster_params.w), kClusterTilesY - 1u);
  uint tz = uint(clamp(log2(max(view_z, 1e-3)) * frame.cluster_params.x +
                       frame.cluster_params.y, 0.0, float(kClusterSlices - 1u)));
  return (tz * kClusterTilesY + ty) * kClusterTilesX + tx;
}

// Clustered decals (share the froxel grid with the lights).
struct Decal {
  float4 row0;  // world -> unit box rows
  float4 row1;
  float4 row2;
  float4 uv_rect;     // atlas uv scale.xy offset.zw
  float4 tint_blend;  // rgb tint, w albedo blend
  float4 params2;     // x normal strength, y roughness mult, z emissive
};
[[vk::binding(15, 2)]] StructuredBuffer<Decal> decal_buffer : register(t15, space2);
[[vk::binding(16, 2)]] StructuredBuffer<uint> decal_cluster_indices : register(t16, space2);
[[vk::combinedImageSampler]] [[vk::binding(17, 2)]] Texture2D decal_atlas : register(t17, space2);
[[vk::combinedImageSampler]] [[vk::binding(17, 2)]] SamplerState decal_atlas_sampler : register(s17, space2);
static const uint kMaxDecalsPerCluster = 16;

// Decal channel atlas: rgb = normal in decal-box space, sharing the albedo
// atlas's uv layout (the albedo alpha is the shared mask).
[[vk::combinedImageSampler]] [[vk::binding(22, 2)]] Texture2D decal_normal_atlas : register(t22, space2);
[[vk::combinedImageSampler]] [[vk::binding(22, 2)]] SamplerState decal_normal_sampler : register(s22, space2);
// Hybrid ReSTIR DI outputs: demodulated diffuse irradiance (multiply by
// albedo/pi) and F-less specular (multiply by f0). Black when off.
[[vk::combinedImageSampler]] [[vk::binding(23, 2)]] Texture2D restir_diffuse_map : register(t23, space2);
[[vk::combinedImageSampler]] [[vk::binding(23, 2)]] SamplerState restir_diffuse_sampler : register(s23, space2);
[[vk::combinedImageSampler]] [[vk::binding(24, 2)]] Texture2D restir_spec_map : register(t24, space2);
[[vk::combinedImageSampler]] [[vk::binding(24, 2)]] SamplerState restir_spec_sampler : register(s24, space2);
// Virtual texturing (slots 25-27): feedback request buffer, mip-mapped page
// indirection (nearest-sampled), physical page atlas. Constants mirror
// VirtualTexture in virtual_texture.h.
[[vk::binding(25, 2)]] RWStructuredBuffer<uint> vt_feedback : register(u25, space2);
[[vk::combinedImageSampler]] [[vk::binding(26, 2)]] Texture2D vt_indirection : register(t26, space2);
[[vk::combinedImageSampler]] [[vk::binding(26, 2)]] SamplerState vt_indirection_sampler : register(s26, space2);
[[vk::combinedImageSampler]] [[vk::binding(27, 2)]] Texture2D vt_atlas : register(t27, space2);
[[vk::combinedImageSampler]] [[vk::binding(27, 2)]] SamplerState vt_atlas_sampler : register(s27, space2);

static const float kVtPagesX = 256.0;      // mip-0 pages per axis
static const float kVtPayload = 120.0;     // payload texels per page
static const float kVtPageStored = 128.0;  // page + border texels
static const float kVtBorder = 4.0;
static const float kVtAtlasPages = 32.0;
static const float kVtMaxMip = 8.0;
static const uint kVtFeedbackCapacity = 16384u;

// Samples the virtual albedo through the indirection (which always points at
// the finest RESIDENT ancestor of the wanted page) and appends this pixel's
// wanted page to the feedback buffer from a sparse rotating pixel subset.
float3 SampleVirtualAlbedo(float2 uv, float2 sv_pos) {
  uv = frac(uv);
  float2 vtexel = uv * (kVtPagesX * kVtPayload);
  float2 duv_dx = ddx(vtexel);
  float2 duv_dy = ddy(vtexel);
  float rho = max(length(duv_dx), length(duv_dy));
  float mip = clamp(floor(log2(max(rho, 1.0))), 0.0, kVtMaxMip);

  uint2 pix = uint2(sv_pos);
  uint frame_index = uint(frame.misc.w);
  if (((pix.x + pix.y * 3u + frame_index) & 63u) == 0u) {
    uint pages = uint(kVtPagesX) >> uint(mip);
    uint2 page = min(uint2(uv * float(pages)), (pages - 1u).xx);
    uint packed = (uint(mip) << 24) | (page.y << 12) | page.x;
    uint slot;
    InterlockedAdd(vt_feedback[0], 1u, slot);
    if (slot < kVtFeedbackCapacity) vt_feedback[1u + slot] = packed;
  }

  float4 entry = vt_indirection.SampleLevel(vt_indirection_sampler, uv, mip);
  if (entry.a < 0.5) return float3(0.35, 0.35, 0.35);  // nothing resident yet
  float src_mip = floor(entry.b * 255.0 + 0.5);
  float pages_at = kVtPagesX / exp2(src_mip);
  float2 in_page = frac(uv * pages_at);
  float2 origin = floor(entry.rg * 255.0 + 0.5) * kVtPageStored;
  float2 atlas_uv =
      (origin + kVtBorder + in_page * kVtPayload) / (kVtAtlasPages * kVtPageStored);
  return vt_atlas.SampleGrad(vt_atlas_sampler, atlas_uv,
                             duv_dx / (kVtAtlasPages * kVtPageStored) / exp2(src_mip),
                             duv_dy / (kVtAtlasPages * kVtPageStored) / exp2(src_mip)).rgb;
}



// LTC fit tables for rect area lights (Heitz et al. 2016): 18 = inverse
// transform matrix entries, 19 = (magnitude, fresnel, -, sphere factor).
[[vk::combinedImageSampler]] [[vk::binding(18, 2)]] Texture2D ltc_matrix_lut : register(t18, space2);
[[vk::combinedImageSampler]] [[vk::binding(18, 2)]] SamplerState ltc_matrix_sampler : register(s18, space2);
[[vk::combinedImageSampler]] [[vk::binding(19, 2)]] Texture2D ltc_amp_lut : register(t19, space2);
[[vk::combinedImageSampler]] [[vk::binding(19, 2)]] SamplerState ltc_amp_sampler : register(s19, space2);

// Local light shadows: a depth atlas of faces (spot = 1, point = 6 cube
// faces); a light's params.w carries 1 + its first face index, 0 = none.
struct LocalShadowFace {
  column_major float4x4 view_proj;
  float4 rect;  // atlas uv scale.xy offset.zw
};
[[vk::binding(20, 2)]] StructuredBuffer<LocalShadowFace> local_shadow_faces : register(t20, space2);
[[vk::combinedImageSampler]] [[vk::binding(21, 2)]] Texture2D local_shadow_atlas : register(t21, space2);
[[vk::combinedImageSampler]] [[vk::binding(21, 2)]] SamplerComparisonState local_shadow_sampler : register(s21, space2);


// Clustered decals: albedo blend plus optional normal perturbation (decal-box
// basis), roughness override and emissive add. Runs before lighting so decals
// shade like part of the material.
void ApplyDecals(inout float3 albedo, inout float3 n, inout float rough_mult,
                 inout float3 emissive, float3 world_pos, float2 sv_xy, float view_z) {
  uint cluster = ClusterOf(sv_xy, view_z);
  uint dcount = min(cluster_counts[cluster] >> 16, kMaxDecalsPerCluster);
  for (uint i = 0; i < dcount; ++i) {
    Decal d = decal_buffer[decal_cluster_indices[cluster * kMaxDecalsPerCluster + i]];
    float3 local = float3(dot(d.row0.xyz, world_pos) + d.row0.w,
                          dot(d.row1.xyz, world_pos) + d.row1.w,
                          dot(d.row2.xyz, world_pos) + d.row2.w);
    if (any(abs(local) > 1.0)) continue;
    // Reject steep surfaces (project along the box -z) and fade the box rim.
    float3 proj_dir = normalize(d.row2.xyz);
    float facing = dot(n, proj_dir);
    if (facing < 0.25) continue;
    float2 uv = local.xy * 0.5 + 0.5;
    float2 atlas_uv = uv * d.uv_rect.xy + d.uv_rect.zw;
    float4 sample_c = decal_atlas.Sample(decal_atlas_sampler, atlas_uv);
    float edge = saturate((1.0 - max(abs(local.x), abs(local.y))) * 6.0) *
                 saturate((1.0 - abs(local.z)) * 3.0);
    float w = saturate(sample_c.a * d.tint_blend.w * edge * saturate((facing - 0.25) / 0.5));
    if (w <= 0.001) continue;
    float3 decal_color = sample_c.rgb * d.tint_blend.rgb;
    albedo = lerp(albedo, decal_color, w);
    // Normal channel: the second atlas holds a normal in the decal's box
    // basis (x/y tangents, z the projection direction).
    if (d.params2.x > 0.001) {
      float3 tn = decal_normal_atlas.Sample(decal_normal_sampler, atlas_uv).rgb * 2.0 - 1.0;
      float3 dn = normalize(tn.x * normalize(d.row0.xyz) + tn.y * normalize(d.row1.xyz) +
                            tn.z * proj_dir);
      n = normalize(lerp(n, dn, w * d.params2.x));
    }
    rough_mult = lerp(rough_mult, d.params2.y, w);
    emissive += decal_color * d.params2.z * w;
  }
}

// NRD REBLUR-denoised stochastic reflections (radiance in rgb); replaces the
// inline mirror trace when kFrameSpecReflTex is set.
[[vk::combinedImageSampler]] [[vk::binding(12, 2)]] Texture2D spec_refl_map : register(t12, space2);
[[vk::combinedImageSampler]] [[vk::binding(12, 2)]] SamplerState spec_refl_sampler : register(s12, space2);

struct MaterialParams {
  float4 base_color_factor;
  float3 emissive_factor;
  float metallic_factor;
  float roughness_factor;
  float alpha_cutoff;
  uint flags;
  float height_scale;  // pom depth, 0 = no march
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
[[vk::binding(0, 1)]] ConstantBuffer<MaterialParams> material : register(b0, space1);

[[vk::combinedImageSampler]] [[vk::binding(1, 1)]] Texture2D base_color_map : register(t1, space1);
[[vk::combinedImageSampler]] [[vk::binding(1, 1)]] SamplerState base_color_sampler : register(s1, space1);
[[vk::combinedImageSampler]] [[vk::binding(2, 1)]] Texture2D normal_map : register(t2, space1);
[[vk::combinedImageSampler]] [[vk::binding(2, 1)]] SamplerState normal_sampler : register(s2, space1);
[[vk::combinedImageSampler]] [[vk::binding(3, 1)]] Texture2D metallic_roughness_map : register(t3, space1);
[[vk::combinedImageSampler]] [[vk::binding(3, 1)]] SamplerState metallic_roughness_sampler : register(s3, space1);
[[vk::combinedImageSampler]] [[vk::binding(5, 1)]] Texture2D height_map : register(t5, space1);
[[vk::combinedImageSampler]] [[vk::binding(5, 1)]] SamplerState height_sampler : register(s5, space1);
[[vk::combinedImageSampler]] [[vk::binding(4, 1)]] Texture2D emissive_map : register(t4, space1);
[[vk::combinedImageSampler]] [[vk::binding(4, 1)]] SamplerState emissive_sampler : register(s4, space1);

[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] TextureCube irradiance_cube : register(t0, space2);
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] SamplerState irradiance_sampler : register(s0, space2);
[[vk::combinedImageSampler]] [[vk::binding(1, 2)]] TextureCube prefiltered_cube : register(t1, space2);
[[vk::combinedImageSampler]] [[vk::binding(1, 2)]] SamplerState prefiltered_sampler : register(s1, space2);
[[vk::combinedImageSampler]] [[vk::binding(2, 2)]] Texture2D brdf_lut : register(t2, space2);
[[vk::combinedImageSampler]] [[vk::binding(2, 2)]] SamplerState brdf_lut_sampler : register(s2, space2);
[[vk::combinedImageSampler]] [[vk::binding(3, 2)]] Texture2D ao_map : register(t3, space2);
[[vk::combinedImageSampler]] [[vk::binding(3, 2)]] SamplerState ao_sampler : register(s3, space2);
[[vk::combinedImageSampler]] [[vk::binding(4, 2)]] Texture2DArray ddgi_irradiance : register(t4, space2);
[[vk::combinedImageSampler]] [[vk::binding(4, 2)]] SamplerState ddgi_irradiance_sampler : register(s4, space2);
[[vk::combinedImageSampler]] [[vk::binding(5, 2)]] Texture2DArray ddgi_distance : register(t5, space2);
[[vk::combinedImageSampler]] [[vk::binding(5, 2)]] SamplerState ddgi_distance_sampler : register(s5, space2);

struct DdgiVolume {
  float4 origin;          // xyz grid origin, w probe spacing
  uint4 counts;           // xyz probe counts, w irradiance texel resolution
  float4 params;          // x distance texel resolution, y hysteresis,
                          // z max ray distance, w energy scale
};
[[vk::binding(6, 2)]] ConstantBuffer<DdgiVolume> ddgi : register(b6, space2);
[[vk::combinedImageSampler]] [[vk::binding(9, 2)]] Texture2D opaque_scene : register(t9, space2);  // for transmission
[[vk::combinedImageSampler]] [[vk::binding(9, 2)]] SamplerState opaque_scene_sampler : register(s9, space2);
[[vk::combinedImageSampler]] [[vk::binding(10, 2)]] Texture2D sun_shadow_map : register(t10, space2);  // SIGMA-denoised
[[vk::combinedImageSampler]] [[vk::binding(10, 2)]] SamplerState sun_shadow_sampler : register(s10, space2);

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
  uint flags;
  float alpha_cutoff;
  float roughness;
  float metallic;
  uint metallic_roughness_texture;
  uint pad0;
  uint pad1;
  uint pad2;
};
[[vk::binding(0, 3)]] StructuredBuffer<MeshRecord> mesh_records : register(t0, space3);
[[vk::binding(1, 3)]] StructuredBuffer<GeometryRecord> geometry_records : register(t1, space3);
[[vk::binding(2, 3)]] StructuredBuffer<MaterialRecord> material_records : register(t2, space3);
[[vk::binding(3, 3)]] Texture2D bindless_textures[] : register(t3, space3);
[[vk::binding(4, 3)]] SamplerState bindless_sampler : register(s4, space3);

static const uint kFlagAlphaMask = 1u;
static const uint kFlagHasNormalMap = 2u;
static const uint kFlagTerrain = 4u;
static const uint kFlagHasHeightMap = 32u;  // 1 << 5
static const uint kFlagSkin = 64u;          // 1 << 6, exports diffuse for screen-space sss
static const uint kFlagHair = 128u;         // 1 << 7, kajiya-kay strand specular
static const uint kFlagVirtualAlbedo = 256u;  // 1 << 8, albedo via the vt atlas
static const uint kFrameIbl = 1u;
static const uint kFrameAoValid = 2u;
static const uint kFrameDdgi = 4u;
static const uint kFrameReflections = 16u;
static const uint kFrameRtShadows = 32u;
static const uint kFrameSigmaShadow = 128u;
static const uint kFrameSpecReflTex = 512u;  // 1 << 9  // sample the denoised sun shadow
static const uint kFrameRestirDi = 1024u;  // 1 << 10, point/spot lights from ReSTIR DI
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
#if REC_SSS_MRT
  // Skin diffuse export for the screen-space sss blur: rgb = the diffuse-only
  // lighting already inside color, a = skin mask. Only the scene-pass pipeline
  // variants have this attachment; the blend pass compiles without it.
  float4 sss : SV_Target2;
#endif
};

// Diffuse-only lighting accumulated by ShadeSurface; main() exports it to the
// sss target for skin materials so the blur can subtract/re-add exactly it.
static float3 g_skin_diffuse = float3(0.0, 0.0, 0.0);

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



// --- parallax occlusion mapping -----------------------------------------
// Height map: r channel, 1 = surface, 0 = height_scale deep. The march digs
// into the surface along the tangent-space view ray; grazing angles get more
// steps. Gradients come from the undisplaced uv so mip selection stays sane.
float2 ParallaxUv(float2 uv, float3 view_ts, float scale, float2 dx, float2 dy) {
  float steps = lerp(28.0, 10.0, saturate(view_ts.z));
  float layer = 1.0 / steps;
  float2 delta = view_ts.xy / max(view_ts.z, 0.08) * scale * layer;
  float depth = 0.0;
  float2 cur = uv;
  float h = 1.0 - height_map.SampleGrad(height_sampler, cur, dx, dy).r;
  float prev_h = h;
  [loop]
  for (uint i = 0; i < 32u && depth < h; ++i) {
    cur -= delta;
    depth += layer;
    prev_h = h;
    h = 1.0 - height_map.SampleGrad(height_sampler, cur, dx, dy).r;
  }
  // Refine: intersect the last linear segment.
  float after = h - depth;
  float before = prev_h - (depth - layer);
  float w = saturate(after / min(after - before, -1e-5));
  return lerp(cur, cur + delta, w);
}

// Soft self-shadow: march from the displaced texel toward the sun and darken
// by the deepest occlusion found (the bricks' mortar lines go dark at low sun).
float ParallaxShadow(float2 uv, float3 light_ts, float scale, float2 dx, float2 dy) {
  if (light_ts.z <= 0.05) return 0.35;
  float h0 = height_map.SampleGrad(height_sampler, uv, dx, dy).r;
  float2 delta = light_ts.xy / light_ts.z * scale / 12.0;
  float occlusion = 0.0;
  float2 cur = uv;
  float ray_h = h0;
  [unroll]
  for (uint i = 0; i < 12u; ++i) {
    cur += delta;
    ray_h += 1.0 / 12.0;
    float h = height_map.SampleGrad(height_sampler, cur, dx, dy).r;
    occlusion = max(occlusion, h - ray_h);
  }
  return 1.0 - saturate(occlusion * 6.0) * 0.75;
}


// --- local light shadows ----------------------------------------------------
// Cube face pick for point lights; order matches kFaceDirs in local_shadows.cc.
uint CubeFaceIndex(float3 d) {
  float3 a = abs(d);
  if (a.x >= a.y && a.x >= a.z) return d.x > 0.0 ? 0u : 1u;
  if (a.y >= a.z) return d.y > 0.0 ? 2u : 3u;
  return d.z > 0.0 ? 4u : 5u;
}

// 2x2 pcf against one atlas face. The faces render with the same matrix the
// sample uses ([0,1] depth, no flip), so this stays self-consistent; the
// normal offset pushes the receiver off its own surface.
float LocalShadow(uint face_index, float3 world_pos, float3 n) {
  LocalShadowFace face = local_shadow_faces[face_index];
  float4 clip = mul(face.view_proj, float4(world_pos + n * 0.04, 1.0));
  if (clip.w <= 0.0) return 1.0;
  float3 ndc = clip.xyz / clip.w;
  float2 uv = ndc.xy * 0.5 + 0.5;
  if (any(uv < 0.0) || any(uv > 1.0) || ndc.z <= 0.0 || ndc.z >= 1.0) return 1.0;
  const float inset = 1.5 / 512.0;  // keep pcf taps inside the face
  uv = clamp(uv, inset, 1.0 - inset) * face.rect.xy + face.rect.zw;
  float ref = ndc.z - 0.0015;
  float texel = face.rect.x / 512.0;
  float sum = 0.0;
  [unroll]
  for (int oy = 0; oy <= 1; ++oy) {
    [unroll]
    for (int ox = 0; ox <= 1; ++ox) {
      sum += local_shadow_atlas.SampleCmpLevelZero(
          local_shadow_sampler, uv + (float2(ox, oy) - 0.5) * texel, ref);
    }
  }
  return sum * 0.25;
}

// --- linearly transformed cosines (Heitz et al. 2016), rect area lights ----
static const float kLtcLut = 64.0;
float3 LtcIntegrateEdge(float3 v1, float3 v2) {
  float x = dot(v1, v2);
  float y = abs(x);
  float a = 0.8543985 + (0.4965155 + 0.0145206 * y) * y;
  float b = 3.4175940 + (4.1616724 + y) * y;
  float v = a / b;
  float theta_sintheta = (x > 0.0) ? v : 0.5 * rsqrt(max(1.0 - x * x, 1e-7)) - v;
  return cross(v1, v2) * theta_sintheta;
}
// Polygon integral of the LTC-transformed clamped cosine; the sphere-factor
// channel of the amplitude table approximates the horizon clipping.
float LtcEvaluate(float3 n, float3 v, float3 pos, float3x3 minv, float3 p0, float3 p1,
                  float3 p2, float3 p3) {
  float3 t1 = normalize(v - n * dot(v, n));
  float3 t2 = cross(n, t1);
  float3x3 m = mul(minv, float3x3(t1, t2, n));
  float3 l0 = normalize(mul(m, p0 - pos));
  float3 l1 = normalize(mul(m, p1 - pos));
  float3 l2 = normalize(mul(m, p2 - pos));
  float3 l3 = normalize(mul(m, p3 - pos));
  float3 vsum = LtcIntegrateEdge(l0, l1) + LtcIntegrateEdge(l1, l2) +
                LtcIntegrateEdge(l2, l3) + LtcIntegrateEdge(l3, l0);
  float len = length(vsum);
  float z = vsum.z / max(len, 1e-7);
  float2 uv = float2(z * 0.5 + 0.5, len);
  uv = uv * ((kLtcLut - 1.0) / kLtcLut) + 0.5 / kLtcLut;
  float scale = ltc_amp_lut.SampleLevel(ltc_amp_sampler, uv, 0.0).w;
  return len * scale;
}

// --- hair strand lobes (Kajiya-Kay with dual shifted highlights) -----------
float3 ShiftTangent(float3 t, float3 n, float shift) {
  return normalize(t + n * shift);
}
float StrandSpecular(float3 t, float3 v, float3 l, float exponent) {
  float3 h = normalize(l + v);
  float tdh = dot(t, h);
  float sin_th = sqrt(max(1.0 - tdh * tdh, 1e-4));
  return smoothstep(-1.0, 0.0, tdh) * pow(sin_th, exponent);
}

// Specular anti-aliasing (Tokuyoshi/Kaplanyan-style): widen the GGX lobe by
// the screen-space variance of the shaded normal, so minified normal maps and
// curved silhouettes stop minting single-pixel fireflies the TAA cannot hold.
// The kernel cap keeps mirrors from degrading into satin at grazing angles.
float SpecularAaRoughness(float roughness, float3 n) {
  float3 dndx = ddx(n);
  float3 dndy = ddy(n);
  float variance = 0.25 * (dot(dndx, dndx) + dot(dndy, dndy));
  float kernel = min(variance, 0.18);
  return sqrt(saturate(roughness * roughness + kernel));
}

// Cook-Torrance ggx with Schlick fresnel and Smith visibility for the sun,
// split-sum ibl with Fdez-Aguera multi-scatter for ambient. Optional clearcoat,
// sheen and anisotropy lobes layer on top.
float3 ShadeSurface(PsIn input, float3 albedo, float3 n, float shadow) {
  float3 v = normalize(frame.camera_position.xyz - input.world_pos);
  if (dot(n, v) < 0.0) n = -n;  // shade double sided geometry from both sides
  float decal_rough_mult = 1.0;
  float3 decal_emissive = float3(0.0, 0.0, 0.0);
  ApplyDecals(albedo, n, decal_rough_mult, decal_emissive, input.world_pos,
              input.sv_position.xy, 0.1 / max(input.sv_position.z, 1e-6));

  // glTF metallic roughness packing: g roughness, b metallic. Terrain reuses
  // this slot as a land layer, so it takes the neutral rough dielectric path.
  float2 mr = (material.flags & kFlagTerrain) != 0u
                  ? float2(1.0, 0.0)
                  : metallic_roughness_map.Sample(metallic_roughness_sampler, input.uv).gb;
  float roughness = clamp(mr.x * material.roughness_factor * decal_rough_mult, 0.045, 1.0);
  roughness = SpecularAaRoughness(roughness, n);
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
  g_skin_diffuse += diffuse_color / kPi * sun * ndl * shadow;

  // Hair: dual shifted Kajiya-Kay strand lobes along the tangent replace the
  // ggx sun response - an uncolored primary at the surface and an
  // albedo-tinted secondary beneath it - with a wrapped diffuse standing in
  // for multiple strand scattering. Point lights below keep the ggx path
  // (the double band only reads under the key light).
  if ((material.flags & kFlagHair) != 0u) {
    float3 strand = input.tangent.xyz - n * dot(input.tangent.xyz, n);
    if (dot(strand, strand) > 1e-8) {
      strand = normalize(strand);
      float gloss = 1.0 - roughness;
      float e1 = lerp(30.0, 260.0, gloss * gloss);
      float3 spec1 = 0.20 * StrandSpecular(ShiftTangent(strand, n, -0.08), v, l, e1).xxx;
      float3 spec2 =
          albedo * 0.45 * StrandSpecular(ShiftTangent(strand, n, 0.10), v, l, e1 * 0.25);
      float wrap = saturate((dot(n, l) + 0.5) / 1.5);
      lit = (diffuse_color / kPi + spec1 + spec2) * wrap * sun * shadow;
    }
  }

  // Subsurface scattering: a wrapped front term softens the terminator and a
  // view-aligned back term glows where light transmits through thin geometry.
  if (material.subsurface > 0.001) {
    float3 sss_h = normalize(l + n * 0.2);
    float back = pow(saturate(dot(v, -sss_h)), 2.0);
    float wrap = saturate((dot(n, l) + 0.4) / 1.4);
    float3 sss_term =
        material.subsurface_color * material.subsurface * (back + 0.4 * wrap) * sun * shadow;
    lit += sss_term;
    g_skin_diffuse += sss_term;
  }

  // Dynamic point lights: ggx + diffuse with smooth inverse-square-ish falloff
  // clamped to the influence radius. light_hits drives the complexity view.
  uint light_hits = 0;
  float pixel_view_z = 0.1 / max(input.sv_position.z, 1e-6);
  uint cluster = ClusterOf(input.sv_position.xy, pixel_view_z);
  uint cluster_count = min(cluster_counts[cluster], kMaxLightsPerCluster);
  for (uint ci = 0; ci < cluster_count; ++ci) {
    uint li = cluster_indices[cluster * kMaxLightsPerCluster + ci];
    PointLight pl = point_lights[li];
    // ReSTIR DI owns point/spot lights this frame (sampled below the loop);
    // area lights keep their analytic LTC / representative-point paths.
    if ((frame.flags & kFrameRestirDi) != 0u && uint(pl.direction_type.w + 0.5) <= 1u) continue;
    float3 to_l = pl.pos_radius.xyz - input.world_pos;
    float dist2 = dot(to_l, to_l);
    float lr = pl.pos_radius.w;
    if (dist2 >= lr * lr) continue;
    float dist = sqrt(max(dist2, 1e-8));
    float3 pl_l = to_l / dist;
    uint ltype = uint(pl.direction_type.w + 0.5);
    float area_norm = 1.0;
    if (ltype == 3u) {
      // Rect panel: exact-ish polygon integration via linearly transformed
      // cosines (spec through the fitted GGX transform, diffuse through the
      // identity), replacing the old representative-point approximation. The
      // shadow ray still tests the panel center.
      float3 ln = normalize(pl.direction_type.xyz);
      if (dot(ln, input.world_pos - pl.pos_radius.xyz) < 0.0) continue;  // behind
      if ((frame.flags & kFrameRtShadows) != 0u) {
        RayDesc pray;
        pray.Origin = input.world_pos + n * 0.02;
        pray.TMin = 0.001;
        pray.Direction = pl_l;
        pray.TMax = dist - 0.04;
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> prq;
        prq.TraceRayInline(tlas, RAY_FLAG_NONE, 0xff, pray);
        prq.Proceed();
        if (prq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) continue;
      }
      float3 lt = normalize(cross(abs(ln.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0), ln));
      float3 lb = cross(ln, lt);
      float3 ex = lt * pl.params.x;
      float3 ey = lb * pl.params.y;
      // Wound so the edge integral's form-factor vector points toward the
      // shaded hemisphere (lb = ln x lt makes the +ey,+ex cycle face away).
      float3 c0 = pl.pos_radius.xyz - ex - ey;
      float3 c1 = pl.pos_radius.xyz - ex + ey;
      float3 c2 = pl.pos_radius.xyz + ex + ey;
      float3 c3 = pl.pos_radius.xyz + ex - ey;
      float2 ltc_uv = float2(roughness, sqrt(saturate(1.0 - ndv)));
      ltc_uv = ltc_uv * ((kLtcLut - 1.0) / kLtcLut) + 0.5 / kLtcLut;
      float4 lm = ltc_matrix_lut.SampleLevel(ltc_matrix_sampler, ltc_uv, 0.0);
      float4 lamp = ltc_amp_lut.SampleLevel(ltc_amp_sampler, ltc_uv, 0.0);
      float3x3 minv = float3x3(float3(lm.x, 0.0, lm.z), float3(0.0, 1.0, 0.0),
                               float3(lm.y, 0.0, lm.w));
      float spec_i = LtcEvaluate(n, v, input.world_pos, minv, c0, c1, c2, c3);
      static const float3x3 kLtcIdentity = {1, 0, 0, 0, 1, 0, 0, 0, 1};
      float diff_i = LtcEvaluate(n, v, input.world_pos, kLtcIdentity, c0, c1, c2, c3);
      float3 spec_col = f0 * lamp.x + (1.0 - f0) * lamp.y;
      float panel_falloff = saturate(1.0 - dist2 / (lr * lr));
      panel_falloff *= panel_falloff;
      ++light_hits;
      float3 panel_diff = diffuse_color * diff_i;
      lit += (panel_diff + spec_col * spec_i) * pl.color_intensity.rgb *
             pl.color_intensity.w * panel_falloff;
      g_skin_diffuse +=
          panel_diff * pl.color_intensity.rgb * pl.color_intensity.w * panel_falloff;
      continue;
    }
    if (ltype == 2u) {
      // Sphere area light through the same LTC path as the rect panels: a
      // light-facing quad proxy with the disk's area (half side r*sqrt(pi)/2)
      // matches the sphere's solid angle closely beyond ~1 radius, replacing
      // the representative-point hack (stretched highlight, no diffuse
      // widening). Radiance semantics match the rect panels.
      float sr = max(pl.params.x, 1e-3);
      float3 sln = -pl_l;  // proxy faces the shaded point
      float3 slt = normalize(cross(abs(sln.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0), sln));
      float3 slb = cross(sln, slt);
      float shalf = sr * 0.8862;  // sqrt(pi)/2: quad area == disk area
      float3 sex = slt * shalf;
      float3 sey = slb * shalf;
      float3 s0 = pl.pos_radius.xyz - sex - sey;
      float3 s1 = pl.pos_radius.xyz - sex + sey;
      float3 s2 = pl.pos_radius.xyz + sex + sey;
      float3 s3 = pl.pos_radius.xyz + sex - sey;
      float2 sltc_uv = float2(roughness, sqrt(saturate(1.0 - ndv)));
      sltc_uv = sltc_uv * ((kLtcLut - 1.0) / kLtcLut) + 0.5 / kLtcLut;
      float4 slm = ltc_matrix_lut.SampleLevel(ltc_matrix_sampler, sltc_uv, 0.0);
      float4 slamp = ltc_amp_lut.SampleLevel(ltc_amp_sampler, sltc_uv, 0.0);
      float3x3 sminv = float3x3(float3(slm.x, 0.0, slm.z), float3(0.0, 1.0, 0.0),
                                float3(slm.y, 0.0, slm.w));
      float sphere_spec = LtcEvaluate(n, v, input.world_pos, sminv, s0, s1, s2, s3);
      static const float3x3 kLtcSphereId = {1, 0, 0, 0, 1, 0, 0, 0, 1};
      float sphere_diff = LtcEvaluate(n, v, input.world_pos, kLtcSphereId, s0, s1, s2, s3);
      float3 sphere_spec_col = f0 * slamp.x + (1.0 - f0) * slamp.y;
      float ball_falloff = saturate(1.0 - dist2 / (lr * lr));
      ball_falloff *= ball_falloff;
      ++light_hits;
      float3 ball_diff = diffuse_color * sphere_diff;
      lit += (ball_diff + sphere_spec_col * sphere_spec) * pl.color_intensity.rgb *
             pl.color_intensity.w * ball_falloff;
      g_skin_diffuse +=
          ball_diff * pl.color_intensity.rgb * pl.color_intensity.w * ball_falloff;
      continue;
    }
    float pndl = max(dot(n, pl_l), 0.0);
    if (pndl <= 0.0) continue;
    ++light_hits;
    // Ray-traced shadow toward the light (rt variant only, where the tlas is
    // bound). Each light gets a hard shadow ray; the sun's soft path is separate.
    float pvis = 1.0;
    if ((frame.flags & kFrameRtShadows) != 0u) {
      RayDesc sray;
      sray.Origin = input.world_pos + n * 0.02;
      sray.TMin = 0.001;
      sray.Direction = pl_l;
      sray.TMax = dist - 0.04;
      RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> srq;
      srq.TraceRayInline(tlas, RAY_FLAG_NONE, 0xff, sray);
      srq.Proceed();
      if (srq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) pvis = 0.0;
    }
    if (pvis <= 0.0) continue;
    float falloff = saturate(1.0 - dist2 / (lr * lr));
    falloff *= falloff;
    if (ltype == 1u) {  // spot cone, squared soft edge
      float cd = dot(-pl_l, normalize(pl.direction_type.xyz));
      float att = saturate((cd - pl.params.y) / max(pl.params.x - pl.params.y, 1e-4));
      falloff *= att * att;
      if (falloff <= 0.0) continue;
    }
    // Local shadow map fallback when the exact per-light rays are off.
    uint shadow_face = uint(pl.params.w + 0.5);
    if (shadow_face != 0u && ltype <= 1u && (frame.flags & kFrameRtShadows) == 0u) {
      uint face = shadow_face - 1u + (ltype == 0u ? CubeFaceIndex(-pl_l) : 0u);
      falloff *= LocalShadow(face, input.world_pos, n);
      if (falloff <= 0.0) continue;
    }
    float3 pl_h = normalize(pl_l + v);
    float pndh = max(dot(n, pl_h), 0.0);
    float pvdh = max(dot(v, pl_h), 0.0);
    float3 pf = f0 + (1.0 - f0) * pow(1.0 - pvdh, 5.0);
    float3 pspec = D_GGX(pndh, a) * V_SmithGGXCorrelated(ndv, pndl, a) * pf * area_norm;
    float3 pdiff = diffuse_color * (1.0 / kPi) * (1.0 - pf);
    lit += (pdiff + pspec) * pl.color_intensity.rgb * pl.color_intensity.w * falloff * pndl;
    g_skin_diffuse += pdiff * pl.color_intensity.rgb * pl.color_intensity.w * falloff * pndl;
  }

  if ((frame.flags & kFrameRestirDi) != 0u) {
    int3 restir_p = int3(input.sv_position.xy, 0);
    float3 restir_di = restir_diffuse_map.Load(restir_p).rgb;
    float3 restir_ds = restir_spec_map.Load(restir_p).rgb;
    float3 restir_diffuse_term = diffuse_color * (1.0 / kPi) * restir_di;
    lit += restir_diffuse_term + f0 * restir_ds;
    g_skin_diffuse += restir_diffuse_term;
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
    if ((frame.flags & kFrameSpecReflTex) != 0u && roughness < frame.reflection_cutoff) {
      // Denoised stochastic reflections: the trace pass VNDF-sampled the GGX
      // lobe, so the texture already carries the roughness-matched blur - no
      // crossfade to the cube needed until the cutoff.
      float3 traced =
          spec_refl_map.SampleLevel(spec_refl_sampler,
                                    input.sv_position.xy / frame.misc.xy, 0.0).rgb;
      float blend = smoothstep(0.75, 1.0, roughness / max(frame.reflection_cutoff, 1e-3));
      radiance = lerp(traced, radiance, blend);
    } else if ((frame.flags & kFrameReflections) != 0u && roughness < frame.reflection_cutoff) {
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
    g_skin_diffuse += (fms_ems + k_d) * irradiance * frame.camera_position.w * ao;
  } else {
    ambient = albedo * frame.sun_color.w;
    g_skin_diffuse += ambient * ao;
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
    case 9:  // raw reflection: the denoised target when present, else traced
      if ((frame.flags & kFrameSpecReflTex) != 0u) {
        return spec_refl_map.SampleLevel(spec_refl_sampler,
                                         input.sv_position.xy / frame.misc.xy, 0.0).rgb;
      }
      return TraceReflection(input.world_pos + n * 0.02, reflect(-v, n));
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
  return lit + ambient + emissive + decal_emissive;
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

// Runtime terrain splat: three land textures tiled at the native land repeat
// (8 tiles per cell), blended by the per-cell weight map in the emissive slot.
float3 TerrainAlbedo(float2 uv) {
  float3 w = emissive_map.Sample(emissive_sampler, uv).rgb;
  float wsum = w.r + w.g + w.b;
  w = wsum > 1e-4 ? w / wsum : float3(1.0, 0.0, 0.0);
  float2 t = uv * 8.0;
  float3 l0 = base_color_map.Sample(base_color_sampler, t).rgb;
  float3 l1 = normal_map.Sample(normal_sampler, t).rgb;
  float3 l2 = metallic_roughness_map.Sample(metallic_roughness_sampler, t).rgb;
  return l0 * w.r + l1 * w.g + l2 * w.b;
}

PsOut main(PsIn input) {
  // Parallax occlusion: displace the uv along the view ray through the height
  // field before anything samples, and self-shadow the sun against it.
  float parallax_sun = 1.0;
  {
    float2 pom_dx = ddx(input.uv);
    float2 pom_dy = ddy(input.uv);
    if ((material.flags & kFlagHasHeightMap) != 0u && material.height_scale > 0.0) {
      float3 gn = normalize(input.normal);
      float3 gt = input.tangent.xyz - gn * dot(input.tangent.xyz, gn);
      if (dot(gt, gt) > 1e-8) {
        gt = normalize(gt);
        float3 gb = cross(gn, gt) * input.tangent.w;
        float3 pv = normalize(frame.camera_position.xyz - input.world_pos);
        float3 view_ts = float3(dot(pv, gt), dot(pv, gb), dot(pv, gn));
        if (view_ts.z > 0.02) {
          input.uv = ParallaxUv(input.uv, view_ts, material.height_scale, pom_dx, pom_dy);
          float3 pl = normalize(-frame.sun_direction.xyz);
          float3 light_ts = float3(dot(pl, gt), dot(pl, gb), dot(pl, gn));
          parallax_sun =
              ParallaxShadow(input.uv, light_ts, material.height_scale, pom_dx, pom_dy);
        }
      }
    }
  }


  float4 base;
  if ((material.flags & kFlagVirtualAlbedo) != 0u) {
    base = float4(SampleVirtualAlbedo(input.uv, input.sv_position.xy), 1.0) *
           material.base_color_factor * input.color;
  } else if ((material.flags & kFlagTerrain) != 0u) {
    base = float4(TerrainAlbedo(input.uv), 1.0) * material.base_color_factor * input.color;
  } else {
    base = base_color_map.Sample(base_color_sampler, input.uv) * material.base_color_factor *
           input.color;
  }
  if ((material.flags & kFlagAlphaMask) != 0u && base.a < material.alpha_cutoff) discard;

  float3 n = SurfaceNormal(input);
  float shadow = SunShadow(input, n) * parallax_sun;

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
#if REC_SSS_MRT
  output.sss = (material.flags & kFlagSkin) != 0u && frame.debug_view == 0u
                   ? float4(g_skin_diffuse, 1.0)
                   : float4(0.0, 0.0, 0.0, 0.0);
#endif
  return output;
}
