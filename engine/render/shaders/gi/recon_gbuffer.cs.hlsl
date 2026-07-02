// SVGF reconstruction path tracer, stage 1: trace one noisy sample per pixel and
// emit the g-buffer + demodulated noisy DIFFUSE IRRADIANCE that the temporal /
// atrous passes reconstruct. Irradiance carries no primary albedo (the composite
// applies albedo/pi), so the denoiser blurs lighting, not texture detail.
//
// Inline ray queries in a compute shader (no DXR raygen/closest-hit), the engine
// path. Self-contained scene access; mirrors pathtrace_gbuffer.cs.hlsl.

struct PathGbufferPush {
  column_major float4x4 inv_view_proj;
  column_major float4x4 view_proj;
  column_major float4x4 prev_view_proj;
  float4 camera_pos;     // xyz eye
  float4 sun_direction;  // xyz travel direction, w intensity
  float4 sun_color;      // rgb, w sun angular radius (radians)
  uint spp;
  float pixel_spread;    // ray-cone spread (radians/pixel) for texture lod
  uint frame_index;
  uint bounces;          // indirect diffuse bounces (>=1)
};
[[vk::push_constant]] PathGbufferPush pc;

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> irradiance_out;
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> normal_rough_out;
[[vk::binding(2, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> viewz_out;
[[vk::binding(3, 0)]] [[vk::image_format("rg16f")]] RWTexture2D<float2> motion_out;
[[vk::binding(4, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> materialid_out;
[[vk::binding(5, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> albedo_out;
[[vk::binding(6, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> emissive_out;
[[vk::binding(7, 0)]] RaytracingAccelerationStructure tlas;
[[vk::combinedImageSampler]] [[vk::binding(8, 0)]] TextureCube sky_cube;
[[vk::combinedImageSampler]] [[vk::binding(8, 0)]] SamplerState sky_sampler;
[[vk::binding(9, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> specular_out;  // noisy

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
  uint metallic_roughness_texture;  // terrain: land layer 2
  uint terrain_layer1_texture;      // terrain: land layer 1
  uint terrain_weight_texture;      // terrain: per-cell weight map
  uint pad2;
};
static const uint kMaterialAlphaMask = 1u;
static const uint kMaterialTerrain = 2u;
[[vk::binding(0, 1)]] StructuredBuffer<MeshRecord> mesh_records;
[[vk::binding(1, 1)]] StructuredBuffer<GeometryRecord> geometry_records;
[[vk::binding(2, 1)]] StructuredBuffer<MaterialRecord> material_records;
[[vk::binding(3, 1)]] Texture2D bindless_textures[];
[[vk::binding(4, 1)]] SamplerState bindless_sampler;

static const float kPi = 3.14159265359;
static const float kInvPi = 0.31830988618;
static const uint kVertexStride = 52;
static const uint kNormalOffset = 12;
static const uint kUvOffset = 40;
static const float kDenoisingRange = 1.0e6;
// Must match the near plane the renderer bakes into PerspectiveReversedZ
// (renderer.cc BuildFrameGraph); viewz below reconstructs from reversed-inf-z.
static const float kNearPlane = 0.1;
static const float kSecondarySpread = 0.03;
static const float kFireflyClamp = 12.0;

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
  float roughness;
  float metallic;
  uint inst;
};

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
  float lod = ConeLod(m.base_color_texture, uvv[0], uvv[1], uvv[2], pos[1] - pos[0], pos[2] - pos[0],
                      cone_width, 1.0);
  float a = m.base_color_factor.a *
            bindless_textures[NonUniformResourceIndex(m.base_color_texture)]
                .SampleLevel(bindless_sampler, uv, clamp(lod, 0.0, 16.0)).a;
  return a >= m.alpha_cutoff;
}

Hit TraceClosest(float3 origin, float3 dir, float cone_spread, bool sample_mr) {
  Hit h;
  h.hit = false;
  h.inst = 0xffffffffu;
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
  // History id for temporal disocclusion: the auto instance INDEX is unique per
  // TLAS instance, unlike InstanceID (== per-mesh bindless index, shared by every
  // instance of the same mesh), so two distinct objects built from one mesh no
  // longer alias each other's lighting history. (Mesh lookup still uses the ID.)
  h.inst = rq.CommittedInstanceIndex();
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
  float3 e1 = mul((float3x3)to_world, pos[1] - pos[0]);
  float3 e2 = mul((float3x3)to_world, pos[2] - pos[0]);
  float ndotd = abs(dot(n, dir));
  bool is_terrain = (m.flags & kMaterialTerrain) != 0u;
  float3 albedo = m.base_color_factor.rgb;
  if (is_terrain) {
    albedo *= TerrainAlbedo(m, uvv[0], uvv[1], uvv[2], uv, e1, e2, cone_spread * hit_t, ndotd);
  } else if (m.base_color_texture != 0xffffffffu) {
    float lod = ConeLod(m.base_color_texture, uvv[0], uvv[1], uvv[2], e1, e2,
                        cone_spread * hit_t, ndotd);
    albedo *= bindless_textures[NonUniformResourceIndex(m.base_color_texture)]
                  .SampleLevel(bindless_sampler, uv, clamp(lod, 0.0, 16.0)).rgb;
  }
  h.albedo = albedo;
  h.emissive = m.emissive;
  // Metallic-roughness map (glTF: .g roughness, .b metallic) scaled by the
  // factors, matching the rasterizer. Only the primary hit needs it (the diffuse
  // bounces don't shade specular), so secondary rays skip the fetch.
  float rough = m.roughness;
  float metal = m.metallic;
  // Terrain reuses the mr slot for land layer 2 (an albedo), so skip the mr
  // fetch and take the neutral rough dielectric path, matching the rasterizer.
  if (sample_mr && !is_terrain && m.metallic_roughness_texture != 0xffffffffu) {
    float lod = ConeLod(m.metallic_roughness_texture, uvv[0], uvv[1], uvv[2], e1, e2,
                        cone_spread * hit_t, ndotd);
    float2 mr = bindless_textures[NonUniformResourceIndex(m.metallic_roughness_texture)]
                    .SampleLevel(bindless_sampler, uv, clamp(lod, 0.0, 16.0)).gb;
    rough *= mr.x;
    metal *= mr.y;
  }
  h.roughness = clamp(rough, 0.045, 1.0);
  h.metallic = saturate(metal);
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

float3 SampleSky(float3 dir) { return min(sky_cube.SampleLevel(sky_sampler, dir, 0.0).rgb, 6.0.xxx); }

float3 SunDir(inout uint rng) {
  float3 l = normalize(-pc.sun_direction.xyz);
  float radius = pc.sun_color.w;
  if (radius <= 0.0) return l;
  float3 up = abs(l.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
  float3 t1 = normalize(cross(up, l));
  float3 t2 = cross(l, t1);
  float a = 2.0 * kPi * Rand(rng);
  float r = sqrt(Rand(rng)) * radius;
  return normalize(l + t1 * (cos(a) * r) + t2 * (sin(a) * r));
}

// Sun direct lighting as IRRADIANCE (Li * NoL), no albedo, no /pi.
float3 DirectIrradiance(float3 pos, float3 normal, inout uint rng) {
  float3 sun = pc.sun_color.rgb * pc.sun_direction.w;
  float3 ldir = SunDir(rng);
  float ndl = dot(normal, ldir);
  if (ndl <= 0.0) return 0.0.xxx;
  if (Occluded(pos + normal * 0.002, ldir, 1000.0, kSecondarySpread)) return 0.0.xxx;
  return sun * ndl;
}

// Analytic GGX specular from the sun (a directional light): noise-free, so it
// goes in the un-denoised emissive channel and stays a sharp highlight. The
// roughness lobe self-attenuates, so matte terrain barely glints while smooth
// surfaces (metal trim, ice, polished wood) get a bright spot.
float D_GGX(float NoH, float a) {
  float a2 = a * a;
  float d = NoH * NoH * (a2 - 1.0) + 1.0;
  return a2 / (kPi * d * d + 1e-7);
}
float V_SmithGGX(float NoV, float NoL, float a) {
  float a2 = a * a;
  float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
  float gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
  return 0.5 / max(gv + gl, 1e-5);
}
float3 F_Schlick(float u, float3 f0) {
  return f0 + (1.0 - f0) * pow(saturate(1.0 - u), 5.0);
}
float3 SunSpecular(float3 pos, float3 N, float3 V, float3 albedo, float rough, float metal) {
  float3 L = normalize(-pc.sun_direction.xyz);
  float NoL = dot(N, L);
  if (NoL <= 0.0) return 0.0.xxx;
  if (Occluded(pos + N * 0.002, L, 1000.0, kSecondarySpread)) return 0.0.xxx;
  float NoV = saturate(dot(N, V)) + 1e-4;
  float3 H = normalize(V + L);
  float NoH = saturate(dot(N, H));
  float VoH = saturate(dot(V, H));
  float a = max(rough * rough, 1e-3);
  float3 f0 = lerp(0.04.xxx, albedo, metal);
  float3 spec = D_GGX(NoH, a) * V_SmithGGX(NoV, saturate(NoL), a) * F_Schlick(VoH, f0);
  return spec * pc.sun_color.rgb * pc.sun_direction.w * saturate(NoL);
}

// Traced specular reflection: one GGX-importance-sampled reflection ray, shaded
// (geometry: emissive + direct; miss: sky), weighted by the NDF-sampling estimator
// F*G*VoH/(NoH*NoV). NOISY (1 spp), so it goes to the separately-denoised specular
// channel. Smooth surfaces have a tight lobe (near-deterministic -> low variance ->
// the denoiser barely blurs it -> sharp reflection); rough surfaces spread out and
// get smoothed. Replaces the prefiltered-sky approximation.
float3 SpecularReflection(float3 pos, float3 N, float3 V, float3 base_color, float rough,
                          float metal, inout uint rng) {
  float NoV = saturate(dot(N, V));
  if (NoV <= 0.0) return 0.0.xxx;
  float a = max(rough * rough, 1e-3);
  // Sample a half-vector from the GGX NDF (Walter 2007).
  float u1 = Rand(rng), u2 = Rand(rng);
  float phi = 2.0 * kPi * u1;
  float ct = sqrt((1.0 - u2) / (1.0 + (a * a - 1.0) * u2));
  float st = sqrt(max(0.0, 1.0 - ct * ct));
  float3 up = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
  float3 t = normalize(cross(up, N));
  float3 b = cross(N, t);
  float3 H = normalize(t * (st * cos(phi)) + b * (st * sin(phi)) + N * ct);
  float3 L = reflect(-V, H);
  float NoL = dot(N, L);
  if (NoL <= 0.0) return 0.0.xxx;

  Hit h = TraceClosest(pos + N * 0.002, L, kSecondarySpread, false);
  float3 Li = h.hit ? (h.emissive + h.albedo * kInvPi * DirectIrradiance(h.position, h.normal, rng))
                    : SampleSky(L);

  float VoH = saturate(dot(V, H));
  float NoH = saturate(dot(N, H));
  float3 f0 = lerp(0.04.xxx, base_color, metal);
  float3 F = F_Schlick(VoH, f0);
  // weight = G*VoH/(NoH*NoV); with V=G/(4 NoL NoV) this is 4*NoL*V*VoH/NoH.
  float w = 4.0 * saturate(NoL) * V_SmithGGX(NoV, saturate(NoL), a) * VoH / (NoH + 1e-4);
  float3 spec = F * w * Li;
  float lum = dot(spec, float3(0.2126, 0.7152, 0.0722));
  if (lum > kFireflyClamp) spec *= kFireflyClamp / lum;
  return spec;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint sw, sh;
  irradiance_out.GetDimensions(sw, sh);
  if (id.x >= sw || id.y >= sh) return;
  uint2 size = uint2(sw, sh);
  uint rng = (id.y * size.x + id.x) * 9781u + pc.frame_index * 26699u + 1u;

  float2 uv = (float2(id.xy) + 0.5) / float2(size);
  float2 ndc = uv * 2.0 - 1.0;
  float4 near_h = mul(pc.inv_view_proj, float4(ndc, 1.0, 1.0));
  float3 ro = pc.camera_pos.xyz;
  float3 primary_dir = normalize(near_h.xyz / near_h.w - ro);

  Hit prim = TraceClosest(ro, primary_dir, pc.pixel_spread, true);

  if (!prim.hit) {
    // Sky: irradiance 0, sky goes to emissive (composite adds it). Far reprojection
    // so motion is valid for the temporal pass.
    float4 prev_clip = mul(pc.prev_view_proj, float4(ro + primary_dir * 1.0e6, 1.0));
    float2 prev_ndc = prev_clip.xy / prev_clip.w;
    irradiance_out[id.xy] = 0.0.xxxx;
    normal_rough_out[id.xy] = float4(0.5, 0.5, 1.0, 1.0);
    viewz_out[id.xy] = kDenoisingRange;
    motion_out[id.xy] = (prev_ndc - ndc) * 0.5;  // engine convention, no y-flip
    materialid_out[id.xy] = 0xffffffffu;
    albedo_out[id.xy] = 0.0.xxxx;
    emissive_out[id.xy] = float4(SampleSky(primary_dir), 1.0);
    specular_out[id.xy] = 0.0.xxxx;
    return;
  }

  // Average spp samples of (primary direct + multi-bounce indirect) irradiance,
  // no primary albedo. Cosine sampling cancels the pdf, leaving throughput that
  // starts at pi and gathers albedo each bounce.
  uint spp = max(pc.spp, 1u);
  uint bounces = max(pc.bounces, 1u);
  float3 irradiance = 0.0.xxx;
  for (uint s = 0; s < spp; ++s) {
    float3 e = DirectIrradiance(prim.position, prim.normal, rng);
    float3 throughput = kPi.xxx;
    float3 pos = prim.position;
    float3 normal = prim.normal;
    for (uint b = 0; b < bounces; ++b) {
      float3 wi = CosineHemisphere(normal, rng);
      Hit h = TraceClosest(pos + normal * 0.002, wi, kSecondarySpread, false);
      if (!h.hit) {
        e += throughput * SampleSky(wi);
        break;
      }
      // L_o(h) without its own indirect (carried by the next bounce).
      e += throughput * (h.emissive + h.albedo * kInvPi * DirectIrradiance(h.position, h.normal, rng));
      throughput *= h.albedo;
      pos = h.position;
      normal = h.normal;
      if (max(throughput.r, max(throughput.g, throughput.b)) < 0.01) break;
    }
    float lum = dot(e, float3(0.2126, 0.7152, 0.0722));
    if (lum > kFireflyClamp) e *= kFireflyClamp / lum;
    irradiance += e;
  }
  irradiance /= float(spp);

  // Sanitize: a single NaN/Inf pixel (degenerate normal, bad bounce) would be
  // spread into a big black rectangle by the a-trous filter. (x >= 0) is false
  // for NaN, so this kills NaN + negatives; min() caps +Inf.
  irradiance.x = irradiance.x >= 0.0 ? irradiance.x : 0.0;
  irradiance.y = irradiance.y >= 0.0 ? irradiance.y : 0.0;
  irradiance.z = irradiance.z >= 0.0 ? irradiance.z : 0.0;
  irradiance = min(irradiance, 1.0e4.xxx);

  // viewZ from reversed-infinite-z depth (positive, == nrd_pack convention).
  float4 clip = mul(pc.view_proj, float4(prim.position, 1.0));
  float depth = clip.z / clip.w;
  float viewz = depth > 0.0 ? kNearPlane / depth : kDenoisingRange;

  // Jitter-free camera motion (static geometry): prevUV - currUV.
  float4 prev_clip = mul(pc.prev_view_proj, float4(prim.position, 1.0));
  float2 prev_ndc = prev_clip.xy / prev_clip.w;
  float2 motion = (prev_ndc - ndc) * 0.5;  // engine convention, no y-flip

  // Two specular contributions, kept apart by noise level:
  //  - analytic sun glint: noise-free direct highlight of the sun -> un-denoised
  //    emissive (a GGX ray rarely hits the tiny sun, so handle it analytically).
  //  - traced reflection of the environment/geometry: noisy -> its own denoised
  //    channel (specular_out).
  float3 V = -primary_dir;
  float3 sun_glint = SunSpecular(prim.position, prim.normal, V, prim.albedo, prim.roughness, prim.metallic);
  // Fade the traced environment reflection out as roughness climbs, matching the
  // rasterizer's reflection_cutoff (mesh_pipeline.h, 0.6). Matte surfaces (grass
  // at ~0.4-0.6 roughness, terrain at 1.0) otherwise pick up a bright denoised sky
  // reflection and read as glossy; their sky lighting is already in the diffuse GI.
  float refl_gate = 1.0 - smoothstep(0.3, 0.6, prim.roughness);
  float3 reflection = refl_gate > 0.0
      ? SpecularReflection(prim.position, prim.normal, V, prim.albedo, prim.roughness,
                           prim.metallic, rng) * refl_gate
      : 0.0.xxx;
  reflection.x = reflection.x >= 0.0 ? reflection.x : 0.0;
  reflection.y = reflection.y >= 0.0 ? reflection.y : 0.0;
  reflection.z = reflection.z >= 0.0 ? reflection.z : 0.0;
  reflection = min(reflection, 1.0e4.xxx);

  irradiance_out[id.xy] = float4(irradiance, 1.0);
  normal_rough_out[id.xy] = float4(prim.normal * 0.5 + 0.5, prim.roughness);
  viewz_out[id.xy] = viewz;
  motion_out[id.xy] = motion;
  materialid_out[id.xy] = prim.inst;
  // Metallic-workflow energy split: metals have no diffuse albedo (their response
  // is all in the specular lobe), dielectrics keep theirs.
  albedo_out[id.xy] = float4(prim.albedo * (1.0 - prim.metallic), 1.0);
  emissive_out[id.xy] = float4(prim.emissive + sun_glint, 1.0);
  specular_out[id.xy] = float4(reflection, 1.0);
}
