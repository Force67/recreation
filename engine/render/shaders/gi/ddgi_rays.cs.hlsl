#include "rhi_bindings.hlsli"
// DDGI probe rays: every probe shoots a rotated fibonacci sphere of rays
// through the TLAS. Misses sample the sky; hits fetch their triangle's
// normal, uv and material through the bindless scene tables and shade from
// the sun (shadow tested), emissive, and the previous frame's irradiance
// for infinite material-colored bounces.

struct DdgiVolume {
  float4 origin;  // xyz grid origin, w probe spacing
  uint4 counts;   // xyz probe counts, w irradiance texel resolution
  float4 params;  // x distance texels, y hysteresis, z max ray distance, w energy scale
};

static const float kPi = 3.14159265359;

// Spherical fibonacci direction for ray i of n, rotated per frame.
float3 FibonacciDir(uint i, uint n) {
  float phi = 2.0 * kPi * frac(i * 0.61803398875);
  float cos_theta = 1.0 - (2.0 * i + 1.0) / n;
  float sin_theta = sqrt(saturate(1.0 - cos_theta * cos_theta));
  return float3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);
}

float2 OctEncode(float3 d) {
  d /= (abs(d.x) + abs(d.y) + abs(d.z));
  float2 o = d.xz;
  if (d.y < 0.0) o = (1.0 - abs(d.zx)) * float2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
  return o;
}

float3 OctDecode(float2 o) {
  float3 d = float3(o.x, 1.0 - abs(o.x) - abs(o.y), o.y);
  if (d.y < 0.0) {
    float2 sign_xz = float2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
    d.xz = (1.0 - abs(d.zx)) * sign_xz;
  }
  return normalize(d);
}

uint3 ProbeFromIndex(uint index, uint3 counts) {
  return uint3(index % counts.x, (index / counts.x) % counts.y, index / (counts.x * counts.y));
}

float3 ProbePosition(uint3 probe, DdgiVolume volume) {
  return volume.origin.xyz + float3(probe) * volume.origin.w;
}

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> rays_out : register(u0, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] TextureCube sky : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState sky_sampler : register(s1, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture2DArray prev_irradiance : register(t2, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState prev_irradiance_sampler : register(s2, space0);
[[vk::binding(3, 0)]] RaytracingAccelerationStructure tlas : register(t3, space0);
[[vk::binding(4, 0)]] ConstantBuffer<DdgiVolume> volume : register(b4, space0);

// Bindless scene tables (set 1), written by the renderer at upload time.
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
[[vk::binding(0, 1)]] StructuredBuffer<MeshRecord> mesh_records : register(t0, space1);
[[vk::binding(1, 1)]] StructuredBuffer<GeometryRecord> geometry_records : register(t1, space1);
[[vk::binding(2, 1)]] StructuredBuffer<MaterialRecord> material_records : register(t2, space1);
[[vk::binding(3, 1)]] Texture2D bindless_textures[] : register(t3, space1);
[[vk::binding(4, 1)]] SamplerState bindless_sampler : register(s4, space1);

static const uint kVertexStride = 52;  // asset::Vertex
static const uint kNormalOffset = 12;
static const uint kUvOffset = 40;

struct PushData {
  float4 rotation_x;  // rows of the per frame rotation
  float4 rotation_y;
  float4 rotation_z;
  float4 sun_direction;  // xyz travel dir, w intensity
  float4 sun_color;      // rgb, w rays per probe
};
PUSH_CONSTANTS(PushData, push);


float2 ProbeAtlasUv(uint3 probe, float3 dir, float texels, float2 atlas_size) {
  float2 oct = OctEncode(dir) * 0.5 + 0.5;
  float2 base = float2(probe.x + probe.z * volume.counts.x, probe.y) * (texels + 2.0) + 1.0;
  return (base + oct * texels) / atlas_size;
}

float3 PrevIrradiance(float3 world_pos, float3 n) {
  float3 local = clamp((world_pos - volume.origin.xyz) / volume.origin.w,
                       0.0.xxx, float3(volume.counts.xyz) - 1.001);
  uint3 probe = (uint3)round(local);  // nearest probe is enough for bounce
  float texels = (float)volume.counts.w;
  float2 atlas = float2((volume.counts.w + 2) * volume.counts.x * volume.counts.z,
                        (volume.counts.w + 2) * volume.counts.y);
  float3 irr = prev_irradiance
      .SampleLevel(prev_irradiance_sampler,
                   float3(ProbeAtlasUv(probe, n, texels, atlas), 0.0), 0.0).rgb;
  return irr * irr;  // stored in perceptual space
}

[numthreads(32, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint ray_count = (uint)push.sun_color.w;
  uint probe_count = volume.counts.x * volume.counts.y * volume.counts.z;
  if (id.x >= ray_count || id.y >= probe_count) return;

  uint3 probe = ProbeFromIndex(id.y, volume.counts.xyz);
  float3 origin = ProbePosition(probe, volume);
  float3 fib = FibonacciDir(id.x, ray_count);
  float3 dir = normalize(float3(dot(push.rotation_x.xyz, fib), dot(push.rotation_y.xyz, fib),
                                dot(push.rotation_z.xyz, fib)));

  RayDesc ray;
  ray.Origin = origin;
  ray.TMin = 0.0;
  ray.Direction = dir;
  ray.TMax = volume.params.z;
  RayQuery<RAY_FLAG_FORCE_OPAQUE> rq;
  rq.TraceRayInline(tlas, RAY_FLAG_NONE, 0xff, ray);
  rq.Proceed();

  float3 radiance;
  float distance = volume.params.z;
  if (rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
    distance = rq.CommittedRayT();
    // Backface hit: probe is inside geometry; mark with negative distance
    // so the blend pass can shorten visibility instead of leaking.
    if (!rq.CommittedTriangleFrontFace()) {
      rays_out[id.xy] = float4(0, 0, 0, -distance * 0.2);
      return;
    }
    float3 hit_pos = origin + dir * distance;

    // Triangle fetch through the bindless tables.
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
    float2 uv = 0.0.xx;
    [unroll]
    for (uint corner = 0; corner < 3; ++corner) {
      uint64_t vertex = mesh.vertex_address + tri[corner] * kVertexStride;
      n_local += vk::RawBufferLoad<float3>(vertex + kNormalOffset, 4) * w[corner];
      uv += vk::RawBufferLoad<float2>(vertex + kUvOffset, 4) * w[corner];
    }
    float3x4 to_world = rq.CommittedObjectToWorld3x4();
    float3 n = normalize(mul((float3x3)to_world, n_local));
    if (dot(n, dir) > 0.0) n = -n;  // shade the side the ray sees

    MaterialRecord material =
        material_records[NonUniformResourceIndex(geometry.material_index)];
    float3 albedo = material.base_color_factor.rgb;
    if (material.base_color_texture != 0xffffffffu) {
      albedo *= bindless_textures[NonUniformResourceIndex(material.base_color_texture)]
                    .SampleLevel(bindless_sampler, uv, 3.0).rgb;
    }

    float3 to_sun = normalize(-push.sun_direction.xyz);
    float ndl = max(dot(n, to_sun), 0.0);
    float shadow = 1.0;
    if (ndl > 0.0) {
      RayDesc shadow_ray;
      shadow_ray.Origin = hit_pos + n * 0.02;
      shadow_ray.TMin = 0.001;
      shadow_ray.Direction = to_sun;
      shadow_ray.TMax = 1000.0;
      RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> srq;
      srq.TraceRayInline(tlas, RAY_FLAG_NONE, 0xff, shadow_ray);
      srq.Proceed();
      if (srq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) shadow = 0.0;
    }
    float3 sun = push.sun_color.rgb * push.sun_direction.w;
    float3 direct = albedo / kPi * sun * ndl * shadow;
    float3 bounce = albedo * PrevIrradiance(hit_pos, n);
    radiance = direct + bounce + material.emissive;
  } else {
    radiance = sky.SampleLevel(sky_sampler, dir, 0).rgb;
  }
  rays_out[id.xy] = float4(radiance, distance);
}
