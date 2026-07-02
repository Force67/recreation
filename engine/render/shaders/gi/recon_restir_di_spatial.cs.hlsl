#include "rhi_bindings.hlsli"
// ReSTIR DI, stage 2: spatial reservoir merge + ONE alpha-tested shadow ray
// for the winning light sample, shaded into the demodulated direct-irradiance
// channel the recon pipeline consumes (sun: Li * cos; point light: windowed-
// falloff Li * cos, matching mesh.ps). The merged, visibility-checked
// reservoir feeds back as next frame's temporal history. Discrete lights, the
// (directional) sun and the (infinitely distant) sky need no reuse Jacobian:
// the target is re-evaluated exactly at the receiving pixel.
struct ReconRestirDiSpatialPush {
  float4 sun_direction;  // xyz travel direction, w intensity
  float4 sun_color;      // rgb, w sun angular radius (radians)
  uint2 size;
  uint frame_index;
  uint light_count;
  uint sample_count;  // spatial neighbor taps
  float radius;       // neighbor disk radius, pixels
  float pad0;
  float pad1;
};
// Reservoir A ids: 0 sun, 1+i point light i, -1 none. Reservoir B (t13/t14,
// u15/u16) carries the sky (id -2 in R2.w); both winners get their own
// alpha-tested shadow ray and the shaded contributions sum.
PUSH_CONSTANTS(ReconRestirDiSpatialPush, pc);

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> direct_out : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float4> r0_in : register(t1, space0);   // dir + light id
[[vk::binding(2, 0)]] Texture2D<float4> r1_in : register(t2, space0);   // w_sum, M, W
[[vk::binding(3, 0)]] Texture2D<float4> p_pos : register(t3, space0);   // primary pos (.w 0 = sky)
[[vk::binding(4, 0)]] Texture2D<float4> curr_nr : register(t4, space0);
[[vk::binding(5, 0)]] Texture2D<float> curr_viewz : register(t5, space0);
[[vk::binding(6, 0)]] Texture2D<uint> curr_matid : register(t6, space0);
struct PointLight {
  float4 pos_radius;       // xyz position, w influence radius
  float4 color_intensity;  // rgb color, w intensity
  float4 direction_type;   // xyz emit direction, w type (0 point 1 spot 2/3 area)
  float4 params;           // spot cos inner/outer; area extents
};
[[vk::binding(7, 0)]] StructuredBuffer<PointLight> point_lights : register(t7, space0);
[[vk::binding(8, 0)]] RaytracingAccelerationStructure tlas : register(t8, space0);
// Post-merge reservoir history for next frame's temporal stage.
[[vk::binding(9, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> r0_out : register(u9, space0);
[[vk::binding(10, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> r1_out : register(u10, space0);
[[vk::combinedImageSampler]] [[vk::binding(11, 0)]] TextureCube sky_cube : register(t11, space0);
[[vk::combinedImageSampler]] [[vk::binding(11, 0)]] SamplerState sky_sampler : register(s11, space0);
[[vk::binding(12, 0)]] StructuredBuffer<float> sky_cdf : register(t12, space0);
[[vk::binding(13, 0)]] Texture2D<float4> r2_in : register(t13, space0);   // sky dir + id
[[vk::binding(14, 0)]] Texture2D<float4> r3_in : register(t14, space0);   // sky w_sum, M, W
[[vk::binding(15, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> r2_out : register(u15, space0);
[[vk::binding(16, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> r3_out : register(u16, space0);

// Bindless scene tables for alpha-tested shadow rays (same layout as the
// recon gbuffer; foliage cutouts must keep casting correct shadows).
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
static const uint kMaterialAlphaMask = 1u;
[[vk::binding(0, 1)]] StructuredBuffer<MeshRecord> mesh_records : register(t0, space1);
[[vk::binding(1, 1)]] StructuredBuffer<GeometryRecord> geometry_records : register(t1, space1);
[[vk::binding(2, 1)]] StructuredBuffer<MaterialRecord> material_records : register(t2, space1);
[[vk::binding(3, 1)]] Texture2D bindless_textures[] : register(t3, space1);
[[vk::binding(4, 1)]] SamplerState bindless_sampler : register(s4, space1);

static const float kPi = 3.14159265359;
static const float kSkyClamp = 6.0;  // matches SampleSky / recon_sky_cdf
static const uint kSkyGridW = 128;
static const uint kSkyGridH = 64;
static const uint kSkyLuma = 1u + kSkyGridH + kSkyGridW * kSkyGridH;
static const uint kVertexStride = 52;
static const uint kUvOffset = 40;
static const float kShadowLod = 4.0;  // fixed mip for shadow-ray alpha tests

uint Pcg(inout uint state) {
  state = state * 747796405u + 2891336453u;
  uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}
float Rand(inout uint state) { return (Pcg(state) & 0xffffffu) / 16777216.0; }

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float3 DecodeN(float4 nr) { return normalize(nr.xyz * 2.0 - 1.0); }
bool InBounds(int2 p) { return all(p >= 0) && all(p < int2(pc.size)); }

float3 SampleSkyClamped(float3 dir) {
  return min(sky_cube.SampleLevel(sky_sampler, dir, 0.0).rgb, kSkyClamp.xxx);
}
float PHatSun(float3 n, float3 disk_dir) {
  return Luma(pc.sun_color.rgb) * pc.sun_direction.w * saturate(dot(n, disk_dir));
}
// p-hat reads the CDF TABLE cell luminance (must match the temporal stage
// bit for bit; see the comment there). Shading below reads the true cube.
float SkyTableLuma(float3 dir) {
  float theta = acos(clamp(dir.y, -1.0, 1.0));
  float phi = atan2(dir.z, dir.x);
  if (phi < 0.0) phi += 2.0 * kPi;
  uint r = min(uint(theta / kPi * float(kSkyGridH)), kSkyGridH - 1u);
  uint c = min(uint(phi / (2.0 * kPi) * float(kSkyGridW)), kSkyGridW - 1u);
  return sky_cdf[kSkyLuma + r * kSkyGridW + c];
}
float PHatSky(float3 n, float3 dir) {
  float ndl = saturate(dot(n, dir));
  if (ndl <= 0.0) return 0.0;
  return SkyTableLuma(dir) * ndl;
}
float PHatLight(float3 x, float3 n, PointLight pl) {
  float3 to_l = pl.pos_radius.xyz - x;
  float dist2 = dot(to_l, to_l);
  float lr = pl.pos_radius.w;
  if (dist2 >= lr * lr) return 0.0;
  float dist = sqrt(max(dist2, 1e-8));
  float ndl = saturate(dot(n, to_l / dist));
  float falloff = saturate(1.0 - dist2 / (lr * lr));
  falloff *= falloff;
  if (pl.direction_type.w >= 0.5 && pl.direction_type.w < 1.5) {  // spot
    float cd = dot(-(to_l / dist), normalize(pl.direction_type.xyz));
    float att = saturate((cd - pl.params.y) / max(pl.params.x - pl.params.y, 1e-4));
    falloff *= att * att;
  }
  return Luma(pl.color_intensity.rgb) * pl.color_intensity.w * falloff * ndl;
}
float PHat(float3 x, float3 n, float light_id, float3 dir) {
  if (light_id < -0.5) return 0.0;  // -1 = empty
  uint id = (uint)round(light_id);
  if (id == 0u) return PHatSun(n, dir);
  if (id - 1u >= pc.light_count) return 0.0;
  return PHatLight(x, n, point_lights[id - 1u]);
}

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
                .SampleLevel(bindless_sampler, uv, kShadowLod).a;
  return a >= m.alpha_cutoff;
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

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  int2 p = int2(tid.xy);
  if (!InBounds(p)) return;

  float4 primary = p_pos.Load(int3(p, 0));
  if (primary.w == 0.0) {  // sky
    direct_out[p] = 0.0.xxxx;
    r0_out[p] = float4(0, 0, 0, -1.0);
    r1_out[p] = 0.0.xxxx;
    r2_out[p] = float4(0, 0, 0, -1.0);
    r3_out[p] = 0.0.xxxx;
    return;
  }
  float3 x = primary.xyz;
  float3 n = DecodeN(curr_nr.Load(int3(p, 0)));
  float vz = curr_viewz.Load(int3(p, 0));
  uint vid = curr_matid.Load(int3(p, 0));
  uint rng = (tid.y * pc.size.x + tid.x) * 30169u + pc.frame_index * 20749u + 17u;

  float4 q0 = r0_in.Load(int3(p, 0));
  float4 q1 = r1_in.Load(int3(p, 0));
  float sel_id = q0.w;
  float3 sel_dir = q0.xyz;
  float w_sum = q1.x;
  float M = q1.y;

  for (uint k = 0; k < pc.sample_count; ++k) {
    float ang = 2.0 * kPi * Rand(rng);
    float rad = pc.radius * sqrt(Rand(rng));
    int2 np = p + int2(round(float2(cos(ang), sin(ang)) * rad));
    if (!InBounds(np) || all(np == p)) continue;
    if (curr_matid.Load(int3(np, 0)) != vid) continue;
    float nz = curr_viewz.Load(int3(np, 0));
    if (abs(nz - vz) / max(vz, 1.0) > 0.1) continue;
    float3 nn = DecodeN(curr_nr.Load(int3(np, 0)));
    if (dot(nn, n) < 0.9) continue;

    float4 n0 = r0_in.Load(int3(np, 0));
    float4 n1 = r1_in.Load(int3(np, 0));
    float nM = n1.y;
    float nW = n1.z;
    if (nM <= 0.0 || nW <= 0.0 || n0.w < -0.5) continue;

    float p_hat = PHat(x, n, n0.w, n0.xyz);
    float w = p_hat * nW * nM;
    M += nM;
    if (!(w > 0.0) || w > 1.0e12) continue;
    w_sum += w;
    if (Rand(rng) < w / w_sum) {
      sel_id = n0.w;
      sel_dir = n0.xyz;
    }
  }

  // Shade the winner through one alpha-tested shadow ray.
  float3 direct = 0.0.xxx;
  float p_hat_sel = PHat(x, n, sel_id, sel_dir);
  float W = (p_hat_sel > 0.0 && M > 0.0) ? w_sum / (M * p_hat_sel) : 0.0;
  if (!(W < 1.0e12)) W = 0.0;
  if (W > 0.0) {
    uint id = (uint)round(sel_id);
    if (id == 0u) {
      if (!Occluded(x + n * 0.002, sel_dir, 1000.0)) {
        direct = pc.sun_color.rgb * pc.sun_direction.w * saturate(dot(n, sel_dir)) * W;
      } else {
        W = 0.0;
      }
    } else if (id - 1u < pc.light_count) {
      PointLight pl = point_lights[id - 1u];
      float3 to_l = pl.pos_radius.xyz - x;
      float dist = length(to_l);
      float3 dir = to_l / max(dist, 1e-4);
      if (!Occluded(x + n * 0.002, dir, max(dist - 0.004, 0.001))) {
        float falloff = saturate(1.0 - (dist * dist) / (pl.pos_radius.w * pl.pos_radius.w));
        falloff *= falloff;
        if (pl.direction_type.w >= 0.5 && pl.direction_type.w < 1.5) {  // spot
          float cd = dot(-dir, normalize(pl.direction_type.xyz));
          float att = saturate((cd - pl.params.y) / max(pl.params.x - pl.params.y, 1e-4));
          falloff *= att * att;
        }
        direct = pl.color_intensity.rgb * pl.color_intensity.w * falloff *
                 saturate(dot(n, dir)) * W;
      } else {
        W = 0.0;
      }
    }
  }

  // --- Reservoir B: sky. Same merge, own shadow ray, contribution adds. ---
  float4 s2 = r2_in.Load(int3(p, 0));
  float4 s3 = r3_in.Load(int3(p, 0));
  float sky_id = s2.w;
  float3 sky_dir = s2.xyz;
  float sky_w_sum = s3.x;
  float sky_M = s3.y;
  for (uint k2 = 0; k2 < pc.sample_count; ++k2) {
    float ang = 2.0 * kPi * Rand(rng);
    float rad = pc.radius * sqrt(Rand(rng));
    int2 np = p + int2(round(float2(cos(ang), sin(ang)) * rad));
    if (!InBounds(np) || all(np == p)) continue;
    if (curr_matid.Load(int3(np, 0)) != vid) continue;
    float nz = curr_viewz.Load(int3(np, 0));
    if (abs(nz - vz) / max(vz, 1.0) > 0.1) continue;
    float3 nn = DecodeN(curr_nr.Load(int3(np, 0)));
    if (dot(nn, n) < 0.9) continue;

    float4 n2 = r2_in.Load(int3(np, 0));
    float4 n3 = r3_in.Load(int3(np, 0));
    if (n3.y <= 0.0 || n3.z <= 0.0 || n2.w > -1.5) continue;
    float p_hat = PHatSky(n, n2.xyz);
    float w = p_hat * n3.z * n3.y;
    sky_M += n3.y;
    if (!(w > 0.0) || w > 1.0e12) continue;
    sky_w_sum += w;
    if (Rand(rng) < w / sky_w_sum) {
      sky_id = -2.0;
      sky_dir = n2.xyz;
    }
  }
  float p_hat_sky = sky_id < -1.5 ? PHatSky(n, sky_dir) : 0.0;
  float sky_W = (p_hat_sky > 0.0 && sky_M > 0.0) ? sky_w_sum / (sky_M * p_hat_sky) : 0.0;
  if (!(sky_W < 1.0e12)) sky_W = 0.0;
  if (sky_W > 0.0) {
    if (!Occluded(x + n * 0.002, sky_dir, 1000.0)) {
      direct += SampleSkyClamped(sky_dir) * saturate(dot(n, sky_dir)) * sky_W;
    } else {
      sky_W = 0.0;
    }
  }

  direct.x = direct.x >= 0.0 ? direct.x : 0.0;
  direct.y = direct.y >= 0.0 ? direct.y : 0.0;
  direct.z = direct.z >= 0.0 ? direct.z : 0.0;
  direct = min(direct, 1.0e4.xxx);
  direct_out[p] = float4(direct, 1.0);
  // Feedback: occluded winners persist with W = 0; the temporal stage skips
  // dead reservoirs and reseeds, so shadowed lights cannot linger.
  r0_out[p] = float4(sel_dir, sel_id);
  r1_out[p] = float4(w_sum, M, W, 0.0);
  r2_out[p] = float4(sky_dir, sky_id);
  r3_out[p] = float4(sky_w_sum, sky_M, sky_W, 0.0);
}
