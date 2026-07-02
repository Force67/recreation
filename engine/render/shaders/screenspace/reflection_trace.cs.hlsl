#include "rhi_bindings.hlsli"
// Stochastic specular reflections for the hybrid renderer: one VNDF-sampled
// GGX ray per pixel through the scene TLAS, shaded like the old inline mirror
// ray (sun + nearest-probe DDGI + emissive at the hit, prefiltered sky on
// miss) PLUS a sun shadow ray at the hit, packed for NRD REBLUR_SPECULAR.
// Replaces the forward pass's deterministic mirror ray: glossy surfaces get a
// real distribution instead of a mirror-to-IBL crossfade, and the denoiser
// eats the sky-hit sparkle foliage used to mint.
//
// NRD.hlsli supplies the radiance packing; keep the CI fallback compiling.
#if __has_include("NRD.hlsli")
#include "NRD.hlsli"
#else
float REBLUR_FrontEnd_GetNormHitDist(float hit_dist, float view_z, float3 params,
                                     float roughness) {
  float smc = (1.0 - exp2(-200.0 * roughness * roughness)) * pow(saturate(roughness), 0.5);
  float f = (params.x + abs(view_z) * params.y) * lerp(params.z, 1.0, smc);
  return max(saturate(hit_dist / f), 1e-6);
}
float4 REBLUR_FrontEnd_PackRadianceAndNormHitDist(float3 radiance, float norm_hit_dist,
                                                  bool sanitize) {
  return float4(radiance, norm_hit_dist);
}
#endif

struct ReflectionPush {
  column_major float4x4 inv_view_proj;  // unjittered
  float4 camera_pos;     // xyz eye
  float4 sun_direction;  // xyz travel direction, w intensity
  float4 sun_color;      // rgb
  float2 inv_size;
  float near_plane;
  float roughness_cutoff;  // above: no trace, ibl only (packed as 0)
  float hit_a;             // REBLUR hit-dist params A, B, C
  float hit_b;
  float hit_c;
  float frame_index;
  uint flags;  // 1 = ddgi volume valid
  float pad0;
  float pad1;
  float pad2;
};
PUSH_CONSTANTS(ReflectionPush, pc);

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> out_radiance : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float> depth_map : register(t1, space0);
[[vk::binding(2, 0)]] Texture2D<float4> normal_map : register(t2, space0);  // oct rg, roughness b
[[vk::binding(3, 0)]] RaytracingAccelerationStructure tlas : register(t3, space0);
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]] TextureCube prefiltered_cube : register(t4, space0);
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]] SamplerState prefiltered_sampler : register(s4, space0);
[[vk::combinedImageSampler]] [[vk::binding(5, 0)]] Texture2DArray ddgi_irradiance : register(t5, space0);
[[vk::combinedImageSampler]] [[vk::binding(5, 0)]] SamplerState ddgi_irradiance_sampler : register(s5, space0);
struct DdgiVolume {
  float4 origin;  // xyz grid origin, w probe spacing
  uint4 counts;   // xyz probe counts, w irradiance texel resolution
  float4 params;  // x distance texels, y hysteresis, z max ray dist, w energy
};
[[vk::binding(6, 0)]] ConstantBuffer<DdgiVolume> ddgi : register(b6, space0);

// Bindless scene tables (set 1, shared layout with the forward rt variant).
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
  uint terrain_layer1_texture;
  uint terrain_weight_texture;
  uint pad2;
};
[[vk::binding(0, 1)]] StructuredBuffer<MeshRecord> mesh_records : register(t0, space1);
[[vk::binding(1, 1)]] StructuredBuffer<GeometryRecord> geometry_records : register(t1, space1);
[[vk::binding(2, 1)]] StructuredBuffer<MaterialRecord> material_records : register(t2, space1);
[[vk::binding(3, 1)]] Texture2D bindless_textures[] : register(t3, space1);
[[vk::binding(4, 1)]] SamplerState bindless_sampler : register(s4, space1);

static const float kPi = 3.14159265359;
static const uint kVertexStride = 52;
static const uint kNormalOffset = 12;
static const uint kUvOffset = 40;
static const uint kFlagDdgi = 1u;

float3 OctDecode(float2 o) {
  float3 d = float3(o.x, 1.0 - abs(o.x) - abs(o.y), o.y);
  if (d.y < 0.0) {
    float2 sign_xz = float2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
    d.xz = (1.0 - abs(d.zx)) * sign_xz;
  }
  return normalize(d);
}
float2 OctEncode(float3 d) {
  d /= (abs(d.x) + abs(d.y) + abs(d.z));
  float2 o = d.xz;
  if (d.y < 0.0) o = (1.0 - abs(d.zx)) * float2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
  return o;
}

float Ign(float2 pixel, float offset) {
  float ign = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
  return frac(ign + offset * 0.61803398875);
}

// Heitz 2018 VNDF sampling: a GGX-visible-normal half vector around n for
// view v, driving the reflection direction. alpha = roughness^2.
float3 SampleGgxVndf(float3 v, float3 n, float alpha, float2 u) {
  float3 up = abs(n.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
  float3 t = normalize(cross(up, n));
  float3 b = cross(n, t);
  float3 ve = float3(dot(v, t), dot(v, b), dot(v, n));

  float3 vh = normalize(float3(alpha * ve.x, alpha * ve.y, ve.z));
  float lensq = vh.x * vh.x + vh.y * vh.y;
  float3 t1 = lensq > 0.0 ? float3(-vh.y, vh.x, 0.0) / sqrt(lensq) : float3(1, 0, 0);
  float3 t2 = cross(vh, t1);
  float r = sqrt(u.x);
  float phi = 2.0 * kPi * u.y;
  float p1 = r * cos(phi);
  float p2 = r * sin(phi);
  float s = 0.5 * (1.0 + vh.z);
  p2 = (1.0 - s) * sqrt(1.0 - p1 * p1) + s * p2;
  float3 nh = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;
  float3 h = normalize(float3(alpha * nh.x, alpha * nh.y, max(0.0, nh.z)));
  return normalize(h.x * t + h.y * b + h.z * n);
}

float2 ProbeAtlasUv(uint3 probe, float3 dir, float texels, float2 atlas_size) {
  float2 oct = OctEncode(dir) * 0.5 + 0.5;
  float2 base = float2(probe.x + probe.z * ddgi.counts.x, probe.y) * (texels + 2.0) + 1.0;
  return (base + oct * texels) / atlas_size;
}

// Nearest-probe diffuse gi at the reflection hit (matches the forward rt
// variant's SampleDdgiNearest).
float3 SampleDdgiNearest(float3 world_pos, float3 n) {
  if ((pc.flags & kFlagDdgi) == 0u) return 0.0.xxx;
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

bool SunOccluded(float3 origin, float3 dir) {
  RayDesc ray;
  ray.Origin = origin;
  ray.TMin = 0.02;
  ray.Direction = dir;
  ray.TMax = 500.0;
  RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> rq;
  rq.TraceRayInline(tlas, RAY_FLAG_NONE, 0xff, ray);
  rq.Proceed();
  return rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint width, height;
  out_radiance.GetDimensions(width, height);
  if (id.x >= width || id.y >= height) return;
  int3 p = int3(id.xy, 0);

  float depth = depth_map.Load(p);
  float4 nr = normal_map.Load(p);
  float roughness = nr.b;
  if (depth <= 0.0 || roughness > pc.roughness_cutoff) {  // sky / ibl-only
    out_radiance[id.xy] = 0.0.xxxx;
    return;
  }

  float2 uv = (float2(id.xy) + 0.5) * pc.inv_size;
  float2 ndc = uv * 2.0 - 1.0;  // vulkan y-down clip, matches rtao/fog
  float4 world_h = mul(pc.inv_view_proj, float4(ndc, depth, 1.0));
  float3 world = world_h.xyz / world_h.w;
  float view_z = pc.near_plane / depth;

  float3 n = OctDecode(nr.rg);
  float3 v = normalize(pc.camera_pos.xyz - world);
  // Double-sided foliage: the prepass exports the geometric facing, which
  // points away from the camera on back faces; VNDF sampling needs v above
  // the surface or it degenerates into fireflies.
  if (dot(n, v) < 0.0) n = -n;
  float2 u = float2(Ign(float2(id.xy), pc.frame_index),
                    Ign(float2(id.xy) + 17.0, pc.frame_index * 1.618));
  float alpha = roughness * roughness;
  float3 h = SampleGgxVndf(v, n, alpha, u);
  float3 dir = reflect(-v, h);
  if (dot(dir, n) <= 0.0) dir = reflect(dir, n);  // rare below-horizon sample

  RayDesc ray;
  ray.Origin = world + n * 0.02;
  ray.TMin = 0.02;
  ray.Direction = dir;
  ray.TMax = 200.0;
  RayQuery<RAY_FLAG_FORCE_OPAQUE> rq;
  rq.TraceRayInline(tlas, RAY_FLAG_NONE, 0xff, ray);
  rq.Proceed();

  float3 radiance;
  float hit_t = 200.0;
  if (rq.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
    radiance = min(prefiltered_cube.SampleLevel(prefiltered_sampler, dir, 1.0).rgb, 8.0.xxx);
  } else {
    hit_t = rq.CommittedRayT();
    float3 hit_pos = ray.Origin + dir * hit_t;
    MeshRecord mesh = mesh_records[NonUniformResourceIndex(rq.CommittedInstanceID())];
    GeometryRecord geometry =
        geometry_records[mesh.geometry_offset + rq.CommittedGeometryIndex()];
    uint64_t index_base =
        mesh.index_address + (geometry.index_offset + rq.CommittedPrimitiveIndex() * 3) * 4;
    uint3 tri;
    tri.x = vk::RawBufferLoad<uint>(index_base);
    tri.y = vk::RawBufferLoad<uint>(index_base + 4);
    tri.z = vk::RawBufferLoad<uint>(index_base + 8);
    float2 bary = rq.CommittedTriangleBarycentrics();
    float3 w = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    float3 n_local = 0.0.xxx;
    float2 huv = 0.0.xx;
    [unroll]
    for (uint corner = 0; corner < 3; ++corner) {
      uint64_t vertex = mesh.vertex_address + tri[corner] * kVertexStride;
      n_local += vk::RawBufferLoad<float3>(vertex + kNormalOffset, 4) * w[corner];
      huv += vk::RawBufferLoad<float2>(vertex + kUvOffset, 4) * w[corner];
    }
    float3x4 to_world = rq.CommittedObjectToWorld3x4();
    float3 hit_n = normalize(mul((float3x3)to_world, n_local));
    if (dot(hit_n, dir) > 0.0) hit_n = -hit_n;

    MaterialRecord hit_material =
        material_records[NonUniformResourceIndex(geometry.material_index)];
    float3 albedo = hit_material.base_color_factor.rgb;
    if (hit_material.base_color_texture != 0xffffffffu) {
      albedo *= bindless_textures[NonUniformResourceIndex(hit_material.base_color_texture)]
                    .SampleLevel(bindless_sampler, huv, 2.0).rgb;
    }
    float3 to_sun = normalize(-pc.sun_direction.xyz);
    float3 sun = pc.sun_color.rgb * pc.sun_direction.w;
    float ndl = max(dot(hit_n, to_sun), 0.0);
    // The old inline ray skipped this shadow ray; shadowed geometry no longer
    // reflects as if sunlit.
    float visible = (ndl > 0.0 && !SunOccluded(hit_pos + hit_n * 0.02, to_sun)) ? 1.0 : 0.0;
    radiance = albedo / kPi * sun * ndl * visible +
               albedo * SampleDdgiNearest(hit_pos, hit_n) + hit_material.emissive;
  }

  float norm_hit = REBLUR_FrontEnd_GetNormHitDist(hit_t, view_z,
                                                  float3(pc.hit_a, pc.hit_b, pc.hit_c),
                                                  roughness);
  out_radiance[id.xy] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance, norm_hit, true);
}
