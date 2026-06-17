// Water surface: fbm wave normals, raytraced reflections shaded through the
// bindless scene tables (sky on miss), screen space refraction with beer
// absorption against the opaque snapshot, fresnel weighting and a ggx sun
// glint. Material base color acts as the absorption tint.

struct FrameGlobals {
  column_major float4x4 view_proj;
  column_major float4x4 prev_view_proj;
  column_major float4x4 inv_view_proj;
  float2 jitter;
  float2 prev_jitter;
  float4 sun_direction;  // xyz travel direction, w intensity
  float4 sun_color;      // rgb, w flat ambient
  float4 camera_position;  // xyz eye, w ibl intensity
  float4 misc;             // x,y render size, z sun radius, w frame index
  uint flags;
  float time;
  float2 pad;
};
[[vk::binding(0, 0)]] ConstantBuffer<FrameGlobals> frame;
[[vk::binding(1, 0)]] RaytracingAccelerationStructure tlas;

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
  float4 origin;
  uint4 counts;
  float4 params;
};
[[vk::binding(6, 2)]] ConstantBuffer<DdgiVolume> ddgi;

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

[[vk::combinedImageSampler]] [[vk::binding(0, 4)]] Texture2D opaque_color;
[[vk::combinedImageSampler]] [[vk::binding(0, 4)]] SamplerState opaque_color_sampler;
[[vk::combinedImageSampler]] [[vk::binding(1, 4)]] Texture2D opaque_depth;
[[vk::combinedImageSampler]] [[vk::binding(1, 4)]] SamplerState opaque_depth_sampler;

static const uint kFrameDdgi = 4u;
static const uint kFrameWaterRt = 8u;
static const float kPi = 3.14159265359;
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

// --- waves -----------------------------------------------------------------

float2 Hash2(float2 p) {
  float3 q = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
  q += dot(q, q.yzx + 33.33);
  return frac((q.xx + q.yz) * q.zy) * 2.0 - 1.0;
}

float GradNoise(float2 p) {
  float2 i = floor(p);
  float2 f = frac(p);
  float2 u = f * f * (3.0 - 2.0 * f);
  float a = dot(Hash2(i), f);
  float b = dot(Hash2(i + float2(1, 0)), f - float2(1, 0));
  float c = dot(Hash2(i + float2(0, 1)), f - float2(0, 1));
  float d = dot(Hash2(i + float2(1, 1)), f - float2(1, 1));
  return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float WaveHeight(float2 p, float t) {
  float h = 0.0;
  float amp = 1.0;
  float freq = 1.0;
  float2 drift = float2(0.13, 0.07);
  [unroll]
  for (int i = 0; i < 4; ++i) {
    h += amp * GradNoise(p * freq + drift * t * freq);
    amp *= 0.45;
    freq *= 2.13;
    drift = float2(-drift.y, drift.x) * 1.3;
  }
  return h;
}

float3 WaveNormal(float2 p, float t, float strength) {
  const float eps = 0.08;
  float h0 = WaveHeight(p, t);
  float hx = WaveHeight(p + float2(eps, 0), t);
  float hz = WaveHeight(p + float2(0, eps), t);
  return normalize(float3(-(hx - h0) / eps * strength, 1.0, -(hz - h0) / eps * strength));
}

// --- ddgi sampling (matches mesh.ps) ----------------------------------------

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

// --- reflection -------------------------------------------------------------

// The sky cube carries the raw sun disk for bloom; reflections must not,
// the analytic glint term owns sun reflection. Blurred mip + clamp.
float3 SkyReflection(float3 dir, float mip) {
  return min(prefiltered_cube.SampleLevel(prefiltered_sampler, dir, mip).rgb, 6.0.xxx);
}

float3 TraceReflection(float3 origin, float3 dir) {
  if ((frame.flags & kFrameWaterRt) == 0u) {
    return SkyReflection(dir, 2.0);
  }
  RayDesc ray;
  ray.Origin = origin + float3(0.0, 0.05, 0.0);
  ray.TMin = 0.01;
  ray.Direction = dir;
  ray.TMax = 500.0;
  RayQuery<RAY_FLAG_FORCE_OPAQUE> rq;
  rq.TraceRayInline(tlas, RAY_FLAG_NONE, 0xff, ray);
  rq.Proceed();
  if (rq.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
    return SkyReflection(dir, 1.0);
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
  float3 n = normalize(mul((float3x3)to_world, n_local));
  if (dot(n, dir) > 0.0) n = -n;

  MaterialRecord hit_material =
      material_records[NonUniformResourceIndex(geometry.material_index)];
  float3 albedo = hit_material.base_color_factor.rgb;
  if (hit_material.base_color_texture != 0xffffffffu) {
    albedo *= bindless_textures[NonUniformResourceIndex(hit_material.base_color_texture)]
                  .SampleLevel(bindless_sampler, uv, 3.0).rgb;
  }
  float3 to_sun = normalize(-frame.sun_direction.xyz);
  float3 sun = frame.sun_color.rgb * frame.sun_direction.w;
  float ndl = max(dot(n, to_sun), 0.0);
  return albedo / kPi * sun * ndl + albedo * SampleDdgiNearest(hit_pos, n) +
         hit_material.emissive;
}

// --- surface ----------------------------------------------------------------

PsOut main(PsIn input) {
  float3 v = normalize(frame.camera_position.xyz - input.world_pos);
  float view_dist = length(frame.camera_position.xyz - input.world_pos);

  // Wave detail fades with distance so far water stays calm under taa.
  float strength = lerp(0.5, 0.08, saturate(view_dist / 400.0)) *
                   saturate(material.roughness_factor * 16.0);
  float3 n = WaveNormal(input.world_pos.xz * 0.7, frame.time * 0.8, strength);

  // Refraction against the opaque snapshot, distorted by the waves.
  float2 screen_uv = input.sv_position.xy / frame.misc.xy;
  float2 distortion = n.xz * (0.035 / max(view_dist * 0.08, 1.0));
  float2 refracted_uv = clamp(screen_uv + distortion, 0.001, 0.999);
  float behind_depth = opaque_depth.SampleLevel(opaque_depth_sampler, refracted_uv, 0).r;
  if (behind_depth > input.sv_position.z) {
    // The distorted sample lands on geometry in front of the water.
    refracted_uv = screen_uv;
    behind_depth = opaque_depth.SampleLevel(opaque_depth_sampler, refracted_uv, 0).r;
  }
  float2 ndc = refracted_uv * 2.0 - 1.0;
  float4 behind_world = mul(frame.inv_view_proj, float4(ndc, max(behind_depth, 1e-7), 1.0));
  behind_world.xyz /= behind_world.w;
  float water_depth = max(length(behind_world.xyz - input.world_pos), 0.0);

  float3 absorption = (1.0 - saturate(material.base_color_factor.rgb)) * 0.9;
  float3 transmittance = exp(-absorption * water_depth);
  float3 scatter = material.base_color_factor.rgb *
                   irradiance_cube.SampleLevel(irradiance_sampler, float3(0, 1, 0), 0).rgb;
  float3 refracted = opaque_color.SampleLevel(opaque_color_sampler, refracted_uv, 0).rgb;
  float3 below = lerp(scatter, refracted, transmittance);

  // Reflection, kept above the surface so the trace never self-hits.
  float3 r = reflect(-v, n);
  r.y = max(r.y, 0.03);
  float3 reflection = TraceReflection(input.world_pos, normalize(r));

  float fresnel = 0.02 + 0.98 * pow(1.0 - max(dot(n, v), 0.0), 5.0);
  float3 color = lerp(below, reflection, fresnel);

  // Sun glint: ggx on the wave normal.
  float3 l = normalize(-frame.sun_direction.xyz);
  float3 h = normalize(l + v);
  float roughness = max(material.roughness_factor, 0.02);
  float a2 = roughness * roughness;
  a2 *= a2;
  float ndh = max(dot(n, h), 0.0);
  float denom = ndh * ndh * (a2 - 1.0) + 1.0;
  float distribution = a2 / max(kPi * denom * denom, 1e-6);
  float ndl = max(dot(n, l), 0.0);
  color += frame.sun_color.rgb * frame.sun_direction.w * distribution * ndl * 0.25 * fresnel;

  PsOut output;
  output.color = float4(color, 1.0);
  float2 curr = input.curr_clip.xy / input.curr_clip.w;
  float2 prev = input.prev_clip.xy / input.prev_clip.w;
  output.motion = (prev - curr) * 0.5;
  return output;
}
