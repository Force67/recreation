#include "rhi_bindings.hlsli"
// Playable path tracer: one sample per pixel, emitting the inputs NRD's
// REBLUR_DIFFUSE denoiser needs instead of brute-force accumulating. The primary
// surface albedo is divided out (demodulation) so the denoiser blurs lighting,
// not texture detail; pathtrace_composite.cs.hlsl re-modulates and adds the
// background back. NRD then reprojects history across camera motion, which is
// what keeps the path-traced view clean while moving.
//
// Self-contained (does not share pathtrace.cs.hlsl's bindings): the 6 NRD-input
// UAVs sit at set 0 bindings 0..5, the shared scene tlas/sky at 6/7, and the
// bindless geometry/material tables at set 1, matching path_tracer.cc.
#include "NRD.hlsli"

struct PathGbufferPush {
  column_major float4x4 inv_view_proj;   // unjittered, for primary ray gen
  column_major float4x4 view_proj;       // unjittered, for viewZ
  column_major float4x4 prev_view_proj;  // unjittered, for camera-motion vectors
  float4 camera_pos;     // xyz eye
  float4 sun_direction;  // xyz travel direction, w intensity
  float4 sun_color;      // rgb, w sun angular radius (radians)
  uint spp;              // lighting samples per pixel (variance down -> NRD stays cleaner)
  float pixel_spread;    // ray-cone spread angle (radians/pixel) for texture mip lod
  uint frame_index;
  uint bounces;
};
PUSH_CONSTANTS(PathGbufferPush, pc);

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> radiance_hitdist_out : register(u0, space0);
[[vk::binding(1, 0)]] [[vk::image_format("rgb10a2")]] RWTexture2D<float4> normal_roughness_out : register(u1, space0);
[[vk::binding(2, 0)]] [[vk::image_format("r16f")]] RWTexture2D<float> viewz_out : register(u2, space0);
[[vk::binding(3, 0)]] [[vk::image_format("rg16f")]] RWTexture2D<float2> motion_out : register(u3, space0);
[[vk::binding(4, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> albedo_out : register(u4, space0);
[[vk::binding(5, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> background_out : register(u5, space0);
[[vk::binding(6, 0)]] RaytracingAccelerationStructure tlas : register(t6, space0);
[[vk::combinedImageSampler]] [[vk::binding(7, 0)]] TextureCube sky_cube : register(t7, space0);
[[vk::combinedImageSampler]] [[vk::binding(7, 0)]] SamplerState sky_sampler : register(s7, space0);

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
  uint flags;  // bit0: alpha mask (cutout)
  float alpha_cutoff;
  float roughness;
  float metallic;
  uint metallic_roughness_texture;  // terrain: land layer 2
  uint terrain_layer1_texture;      // terrain: land layer 1
  uint terrain_weight_texture;      // terrain: per-cell weight map
  uint pad2;
};
static const uint kMaterialAlphaMask = 1u;
static const uint kMaterialTerrain = 2u;
[[vk::binding(0, 1)]] StructuredBuffer<MeshRecord> mesh_records : register(t0, space1);
[[vk::binding(1, 1)]] StructuredBuffer<GeometryRecord> geometry_records : register(t1, space1);
[[vk::binding(2, 1)]] StructuredBuffer<MaterialRecord> material_records : register(t2, space1);
[[vk::binding(3, 1)]] Texture2D bindless_textures[] : register(t3, space1);
[[vk::binding(4, 1)]] SamplerState bindless_sampler : register(s4, space1);

static const float kPi = 3.14159265359;
static const uint kVertexStride = 52;
static const uint kNormalOffset = 12;
static const uint kUvOffset = 40;

uint Pcg(inout uint state) {
  state = state * 747796405u + 2891336453u;
  uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}
float Rand(inout uint state) { return (Pcg(state) & 0xffffffu) / 16777216.0; }

float3 CosineHemisphere(float3 n, inout uint rng) {
  float u1 = Rand(rng);
  float u2 = Rand(rng);
  float r = sqrt(u1);
  float phi = 2.0 * kPi * u2;
  float3 t = abs(n.y) < 0.99 ? normalize(cross(float3(0, 1, 0), n))
                             : normalize(cross(float3(1, 0, 0), n));
  float3 b = cross(n, t);
  return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u1)));
}

struct Hit {
  bool hit;
  float3 position;
  float3 normal;
  float3 albedo;
  float3 emissive;
};

// Alpha-test a candidate hit on a non-opaque (cutout) triangle: foliage and
// grass cards are masked materials, so the ray samples the base color alpha and
// rejects below the cutoff instead of treating the quad as solid.
// Ray-cone texture mip (Akenine-Moller, RT Gems "Texture LOD"). The triangle's
// uv-area / world-area is the texel density; cone_width is the ray footprint
// radius at the hit (spread angle * hit distance). Replaces the mip-0 sampling
// that aliased and crawled under motion. uv0..2 / e1,e2 are the triangle's uvs
// and two world-space edges; ndotd accounts for grazing angles.
float ConeLod(uint tex, float2 uv0, float2 uv1, float2 uv2, float3 e1, float3 e2,
              float cone_width, float ndotd) {
  uint tw, th;
  bindless_textures[NonUniformResourceIndex(tex)].GetDimensions(tw, th);
  float uv_area = abs((uv1.x - uv0.x) * (uv2.y - uv0.y) - (uv2.x - uv0.x) * (uv1.y - uv0.y));
  float world_area = length(cross(e1, e2));
  float density = 0.5 * log2(max(uv_area, 1e-12) * float(tw) * float(th) / max(world_area, 1e-12));
  return density + log2(max(cone_width / max(ndotd, 0.1), 1e-7));
}

// Runtime terrain splat (mirrors mesh_rt.ps TerrainAlbedo): three land layers
// tiled at the native 8x repeat, blended by the per-cell weight map. The land
// layers live in the base-color / layer1 / metallic-roughness bindless slots;
// the weight map in the terrain_weight slot.
float3 TerrainAlbedo(MaterialRecord m, float2 uvv0, float2 uvv1, float2 uvv2, float2 uv,
                     float3 e1, float3 e2, float cone_width, float ndotd) {
  float3 w = bindless_textures[NonUniformResourceIndex(m.terrain_weight_texture)]
                 .SampleLevel(bindless_sampler, uv, 0.0).rgb;
  float wsum = w.r + w.g + w.b;
  w = wsum > 1e-4 ? w / wsum : float3(1.0, 0.0, 0.0);
  float2 t0 = uvv0 * 8.0, t1 = uvv1 * 8.0, t2 = uvv2 * 8.0, tuv = uv * 8.0;
  uint layers[3] = {m.base_color_texture, m.terrain_layer1_texture, m.metallic_roughness_texture};
  float3 albedo = 0.0.xxx;
  [unroll]
  for (uint i = 0; i < 3; ++i) {
    float lod = ConeLod(layers[i], t0, t1, t2, e1, e2, cone_width, ndotd);
    albedo += w[i] * bindless_textures[NonUniformResourceIndex(layers[i])]
                         .SampleLevel(bindless_sampler, tuv, clamp(lod, 0.0, 16.0)).rgb;
  }
  return albedo;
}

bool PassesAlpha(uint inst, uint geom, uint prim, float2 bary, float cone_width) {
  MeshRecord mesh = mesh_records[NonUniformResourceIndex(inst)];
  GeometryRecord geometry = geometry_records[mesh.geometry_offset + geom];
  MaterialRecord m = material_records[NonUniformResourceIndex(geometry.material_index)];
  if ((m.flags & kMaterialAlphaMask) == 0u || m.base_color_texture == 0xffffffffu) return true;
  uint64_t index_base = mesh.index_address + (geometry.index_offset + prim * 3) * 4;
  uint3 tri;
  tri.x = vk::RawBufferLoad<uint>(index_base);
  tri.y = vk::RawBufferLoad<uint>(index_base + 4);
  tri.z = vk::RawBufferLoad<uint>(index_base + 8);
  float3 w = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
  float2 uvv[3];
  float3 pos[3];
  [unroll]
  for (uint c = 0; c < 3; ++c) {
    uint64_t vertex = mesh.vertex_address + tri[c] * kVertexStride;
    pos[c] = vk::RawBufferLoad<float3>(vertex, 4);
    uvv[c] = vk::RawBufferLoad<float2>(vertex + kUvOffset, 4);
  }
  float2 uv = uvv[0] * w[0] + uvv[1] * w[1] + uvv[2] * w[2];
  // Object-space area (foliage instances are ~unit scale); mips the cutout so
  // distant grass/leaves stop aliasing into shimmer.
  float lod = ConeLod(m.base_color_texture, uvv[0], uvv[1], uvv[2], pos[1] - pos[0], pos[2] - pos[0],
                      cone_width, 1.0);
  float a = m.base_color_factor.a *
            bindless_textures[NonUniformResourceIndex(m.base_color_texture)]
                .SampleLevel(bindless_sampler, uv, clamp(lod, 0.0, 16.0))
                .a;
  return a >= m.alpha_cutoff;
}

Hit TraceClosest(float3 origin, float3 dir, float cone_spread) {
  Hit h;
  h.hit = false;
  RayDesc ray;
  ray.Origin = origin;
  ray.TMin = 0.001;
  ray.Direction = dir;
  ray.TMax = 1000.0;
  RayQuery<RAY_FLAG_NONE> rq;
  rq.TraceRayInline(tlas, RAY_FLAG_NONE, 0xff, ray);
  while (rq.Proceed()) {
    if (rq.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
        PassesAlpha(rq.CandidateInstanceID(), rq.CandidateGeometryIndex(),
                    rq.CandidatePrimitiveIndex(), rq.CandidateTriangleBarycentrics(),
                    cone_spread * rq.CandidateTriangleRayT())) {
      rq.CommitNonOpaqueTriangleHit();
    }
  }
  if (rq.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return h;

  float hit_t = rq.CommittedRayT();
  h.hit = true;
  h.position = origin + dir * hit_t;
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
  float3 pos[3];
  float3 nrm[3];
  float2 uvv[3];
  [unroll]
  for (uint c = 0; c < 3; ++c) {
    uint64_t vertex = mesh.vertex_address + tri[c] * kVertexStride;
    pos[c] = vk::RawBufferLoad<float3>(vertex, 4);
    nrm[c] = vk::RawBufferLoad<float3>(vertex + kNormalOffset, 4);
    uvv[c] = vk::RawBufferLoad<float2>(vertex + kUvOffset, 4);
  }
  float3 n_local = nrm[0] * w[0] + nrm[1] * w[1] + nrm[2] * w[2];
  float2 uv = uvv[0] * w[0] + uvv[1] * w[1] + uvv[2] * w[2];
  float3x4 to_world = rq.CommittedObjectToWorld3x4();
  float3 n = normalize(mul((float3x3)to_world, n_local));
  if (dot(n, dir) > 0.0) n = -n;
  h.normal = n;

  MaterialRecord m = material_records[NonUniformResourceIndex(geometry.material_index)];
  // Ray-cone mip so textures stop minification-aliasing into shimmer at range.
  float3 e1 = mul((float3x3)to_world, pos[1] - pos[0]);
  float3 e2 = mul((float3x3)to_world, pos[2] - pos[0]);
  float ndotd = abs(dot(n, dir));
  float3 albedo = m.base_color_factor.rgb;
  if ((m.flags & kMaterialTerrain) != 0u) {
    albedo *= TerrainAlbedo(m, uvv[0], uvv[1], uvv[2], uv, e1, e2, cone_spread * hit_t, ndotd);
  } else if (m.base_color_texture != 0xffffffffu) {
    float lod = ConeLod(m.base_color_texture, uvv[0], uvv[1], uvv[2], e1, e2,
                        cone_spread * hit_t, ndotd);
    albedo *= bindless_textures[NonUniformResourceIndex(m.base_color_texture)]
                  .SampleLevel(bindless_sampler, uv, clamp(lod, 0.0, 16.0)).rgb;
  }
  h.albedo = albedo;
  h.emissive = m.emissive;
  return h;
}

bool Occluded(float3 origin, float3 dir, float dist, float cone_spread) {
  RayDesc ray;
  ray.Origin = origin;
  ray.TMin = 0.001;
  ray.Direction = dir;
  ray.TMax = dist;
  RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> rq;
  rq.TraceRayInline(tlas, RAY_FLAG_NONE, 0xff, ray);
  while (rq.Proceed()) {
    if (rq.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
        PassesAlpha(rq.CandidateInstanceID(), rq.CandidateGeometryIndex(),
                    rq.CandidatePrimitiveIndex(), rq.CandidateTriangleBarycentrics(),
                    cone_spread * rq.CandidateTriangleRayT())) {
      rq.CommitNonOpaqueTriangleHit();
    }
  }
  return rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

float3 SampleSky(float3 dir) {
  return min(sky_cube.SampleLevel(sky_sampler, dir, 0.0).rgb, 6.0.xxx);
}

float3 SunDirection(float3 sun_travel, float radius, inout uint rng) {
  float3 l = normalize(-sun_travel);
  if (radius <= 0.0) return l;
  float3 up = abs(l.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
  float3 t1 = normalize(cross(up, l));
  float3 t2 = cross(l, t1);
  float a = 2.0 * kPi * Rand(rng);
  float r = sqrt(Rand(rng)) * radius;
  return normalize(l + t1 * (cos(a) * r) + t2 * (sin(a) * r));
}

// Sky / invalid pixels report a viewZ beyond this so NRD ignores them. Near
// plane and the REBLUR hit-distance params must match the engine
// (NrdDenoiser::kHitDistParams).
static const float kDenoisingRange = 1.0e6;
static const float kNearPlane = 0.1;
static const float3 kHitDistParams = float3(3.0, 0.1, 20.0);
// Diffuse bounce rays spread to a near-hemisphere; a coarse cone is fine for the
// indirect texture/foliage lod. Firefly clamp keeps rare bright samples from
// poisoning NRD's temporal history (its anti-firefly is a second line of defense).
static const float kSecondarySpread = 0.03;
static const float kFireflyClamp = 8.0;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint sw, sh;
  radiance_hitdist_out.GetDimensions(sw, sh);
  if (id.x >= sw || id.y >= sh) return;
  uint2 size = uint2(sw, sh);
  uint rng = (id.y * size.x + id.x) * 9781u + pc.frame_index * 26699u + 1u;

  float2 ndc = (float2(id.xy) + 0.5) / float2(size) * 2.0 - 1.0;
  float4 near_h = mul(pc.inv_view_proj, float4(ndc, 1.0, 1.0));
  float3 ro = pc.camera_pos.xyz;
  float3 primary_dir = normalize(near_h.xyz / near_h.w - ro);

  float3 sun = pc.sun_color.rgb * pc.sun_direction.w;
  float3 background = 0.0.xxx;  // sky on a primary miss + primary emissive
  float3 prim_albedo = 1.0.xxx;
  float3 prim_normal = float3(0, 0, 1);
  float3 prim_pos = ro;

  // Primary visibility is deterministic, so trace it once; only the lighting
  // (random bounces + sun NEE) is sampled spp times and averaged. More samples
  // = lower input variance = NRD has to invent less under motion = less shimmer.
  Hit prim = TraceClosest(ro, primary_dir, pc.pixel_spread);
  bool primary_hit = prim.hit;
  if (primary_hit) {
    prim_pos = prim.position;
    prim_normal = prim.normal;
    prim_albedo = prim.albedo;
    background += prim.emissive;  // re-added after denoise, never demodulated
  } else {
    background += SampleSky(primary_dir);
  }

  uint spp = max(pc.spp, 1u);
  float3 radiance = 0.0.xxx;  // lighting only (primary albedo demodulated)
  float first_hit_dist = 0.0;
  if (primary_hit) {
    for (uint s = 0; s < spp; ++s) {
      float3 sr = 0.0.xxx;  // this sample's radiance, clamped before accumulation
      float3 throughput = 1.0.xxx;
      float3 normal = prim_normal;
      float3 pos = prim_pos;
      float3 albedo = prim_albedo;
      // Indirect bounces from the primary hit; NEE toward the sun at each vertex.
      for (uint b = 0; b < pc.bounces; ++b) {
        float3 ldir = SunDirection(pc.sun_direction.xyz, pc.sun_color.w, rng);
        float ndl = dot(normal, ldir);
        if (ndl > 0.0 && !Occluded(pos + normal * 0.002, ldir, 1000.0, kSecondarySpread)) {
          sr += throughput * albedo / kPi * sun * ndl;
        }
        float3 dir = CosineHemisphere(normal, rng);
        throughput *= albedo;
        if (max(throughput.r, max(throughput.g, throughput.b)) < 0.01) break;
        Hit h = TraceClosest(pos + normal * 0.002, dir, kSecondarySpread);
        if (b == 0) first_hit_dist += h.hit ? distance(h.position, prim_pos) : 1000.0;
        if (!h.hit) {
          sr += throughput * SampleSky(dir);
          break;
        }
        sr += throughput * h.emissive;
        normal = h.normal;
        pos = h.position;
        albedo = h.albedo;
      }
      // Firefly clamp: rare bright paths otherwise stick in NRD's history as blobs.
      float lum = dot(sr, float3(0.2126, 0.7152, 0.0722));
      if (lum > kFireflyClamp) sr *= kFireflyClamp / lum;
      radiance += sr;
    }
    radiance /= float(spp);
    first_hit_dist /= float(spp);
  }

  float3 demod = radiance / max(prim_albedo, (1e-3).xxx);

  float viewz = kDenoisingRange;
  float2 motion = 0.0.xx;
  if (primary_hit) {
    // Reversed infinite z: ndc depth = near / viewZ, so viewZ = near / depth.
    float4 clip = mul(pc.view_proj, float4(prim_pos, 1.0));
    float depth = clip.z / clip.w;
    viewz = depth > 0.0 ? kNearPlane / depth : kDenoisingRange;
    // Camera-only motion (static geometry): current ndc is this pixel's ndc.
    float4 prev_clip = mul(pc.prev_view_proj, float4(prim_pos, 1.0));
    float2 prev_ndc = prev_clip.xy / prev_clip.w;
    motion = (prev_ndc - ndc) * 0.5;  // uv-space delta, matches the taa motion pass
  }

  float norm_hit = REBLUR_FrontEnd_GetNormHitDist(first_hit_dist, viewz, kHitDistParams, 1.0);
  radiance_hitdist_out[id.xy] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(demod, norm_hit, false);
  normal_roughness_out[id.xy] = NRD_FrontEnd_PackNormalAndRoughness(prim_normal, 1.0, 0.0);
  viewz_out[id.xy] = viewz;
  motion_out[id.xy] = motion;
  albedo_out[id.xy] = float4(prim_albedo, 1.0);
  background_out[id.xy] = float4(background, primary_hit ? 1.0 : 0.0);
}
