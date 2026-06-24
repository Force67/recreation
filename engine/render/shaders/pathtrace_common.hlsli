// Shared path-tracer scene access and ray helpers, included by both the
// reference accumulator (pathtrace.cs.hlsl) and the NRD-input emitter
// (pathtrace_gbuffer.cs.hlsl). Holds the TLAS + bindless geometry/material
// tables + the sky cube, alpha-tested closest-hit / occlusion ray queries, and
// the sampling utilities. The including shader declares its own output UAVs in
// set 0 at bindings 0..7; shared scene resources live at bindings 8+ so they
// never collide with those outputs.
#ifndef RECREATION_PATHTRACE_COMMON_HLSLI
#define RECREATION_PATHTRACE_COMMON_HLSLI

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
};
static const uint kMaterialAlphaMask = 1u;

[[vk::binding(8, 0)]] RaytracingAccelerationStructure tlas;
[[vk::combinedImageSampler]] [[vk::binding(9, 0)]] TextureCube sky_cube;
[[vk::combinedImageSampler]] [[vk::binding(9, 0)]] SamplerState sky_sampler;

[[vk::binding(0, 1)]] StructuredBuffer<MeshRecord> mesh_records;
[[vk::binding(1, 1)]] StructuredBuffer<GeometryRecord> geometry_records;
[[vk::binding(2, 1)]] StructuredBuffer<MaterialRecord> material_records;
[[vk::binding(3, 1)]] Texture2D bindless_textures[];
[[vk::binding(4, 1)]] SamplerState bindless_sampler;

static const float kPi = 3.14159265359;
static const uint kVertexStride = 52;
static const uint kNormalOffset = 12;
static const uint kUvOffset = 40;

// pcg hash based rng in [0,1).
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
bool PassesAlpha(uint inst, uint geom, uint prim, float2 bary) {
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
  float2 uv = 0.0.xx;
  [unroll]
  for (uint c = 0; c < 3; ++c) {
    uint64_t vertex = mesh.vertex_address + tri[c] * kVertexStride;
    uv += vk::RawBufferLoad<float2>(vertex + kUvOffset, 4) * w[c];
  }
  float a = m.base_color_factor.a *
            bindless_textures[NonUniformResourceIndex(m.base_color_texture)]
                .SampleLevel(bindless_sampler, uv, 0.0)
                .a;
  return a >= m.alpha_cutoff;
}

Hit TraceClosest(float3 origin, float3 dir) {
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
                    rq.CandidatePrimitiveIndex(), rq.CandidateTriangleBarycentrics())) {
      rq.CommitNonOpaqueTriangleHit();
    }
  }
  if (rq.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return h;

  h.hit = true;
  h.position = origin + dir * rq.CommittedRayT();
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
  for (uint c = 0; c < 3; ++c) {
    uint64_t vertex = mesh.vertex_address + tri[c] * kVertexStride;
    n_local += vk::RawBufferLoad<float3>(vertex + kNormalOffset, 4) * w[c];
    uv += vk::RawBufferLoad<float2>(vertex + kUvOffset, 4) * w[c];
  }
  float3x4 to_world = rq.CommittedObjectToWorld3x4();
  float3 n = normalize(mul((float3x3)to_world, n_local));
  if (dot(n, dir) > 0.0) n = -n;
  h.normal = n;

  MaterialRecord m = material_records[NonUniformResourceIndex(geometry.material_index)];
  float3 albedo = m.base_color_factor.rgb;
  if (m.base_color_texture != 0xffffffffu) {
    albedo *= bindless_textures[NonUniformResourceIndex(m.base_color_texture)]
                  .SampleLevel(bindless_sampler, uv, 0.0).rgb;
  }
  h.albedo = albedo;
  h.emissive = m.emissive;
  return h;
}

bool Occluded(float3 origin, float3 dir, float dist) {
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
                    rq.CandidatePrimitiveIndex(), rq.CandidateTriangleBarycentrics())) {
      rq.CommitNonOpaqueTriangleHit();
    }
  }
  return rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

float3 SampleSky(float3 dir) {
  // Clamp suppresses the raw sun disk; direct sun comes from the nee term.
  return min(sky_cube.SampleLevel(sky_sampler, dir, 0.0).rgb, 6.0.xxx);
}

// Sample a direction toward the soft sun disk. sun_travel is the light's travel
// direction (so the direction *to* the sun is -sun_travel); radius is in radians.
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

#endif  // RECREATION_PATHTRACE_COMMON_HLSLI
