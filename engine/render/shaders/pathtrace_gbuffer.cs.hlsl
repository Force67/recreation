// Playable path tracer: one sample per pixel, emitting the inputs NRD's
// REBLUR_DIFFUSE denoiser needs instead of brute-force accumulating. The
// primary surface albedo is divided out (demodulation) so the denoiser blurs
// lighting, not texture detail; pathtrace_composite.cs.hlsl re-modulates and
// adds the background back. NRD then reprojects history across camera motion,
// which is what makes the path-traced view stay clean while moving.

// NRD supplies the packing helpers. It is an optional dependency, so fall back
// to trivial encodings when it is not vendored; this path is gated off at
// runtime in that case but the shader must still compile.
#if __has_include("NRD.hlsli")
#include "NRD.hlsli"
#else
float REBLUR_FrontEnd_GetNormHitDist(float hit, float vz, float3 p, float r) {
  return max(saturate(hit / max(p.x + abs(vz) * p.y, 1e-6)), 1e-6);
}
float4 REBLUR_FrontEnd_PackRadianceAndNormHitDist(float3 rad, float h, bool s) {
  return float4(rad, h);
}
float4 NRD_FrontEnd_PackNormalAndRoughness(float3 n, float r, float m) {
  return float4(n * 0.5 + 0.5, r);
}
#endif

struct PathGbufferPush {
  column_major float4x4 inv_view_proj;   // unjittered, for primary ray gen
  column_major float4x4 view_proj;       // unjittered, for viewZ
  column_major float4x4 prev_view_proj;  // unjittered, for camera-motion vectors
  float4 camera_pos;     // xyz eye
  float4 sun_direction;  // xyz travel direction, w intensity
  float4 sun_color;      // rgb, w sun angular radius (radians)
  uint2 size;
  uint frame_index;
  uint bounces;
};
[[vk::push_constant]] PathGbufferPush pc;

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> radiance_hitdist_out;
[[vk::binding(1, 0)]] [[vk::image_format("rgb10a2")]] RWTexture2D<float4> normal_roughness_out;
[[vk::binding(2, 0)]] [[vk::image_format("r16f")]] RWTexture2D<float> viewz_out;
[[vk::binding(3, 0)]] [[vk::image_format("rg16f")]] RWTexture2D<float2> motion_out;
[[vk::binding(4, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> albedo_out;
[[vk::binding(5, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> background_out;

#include "pathtrace_common.hlsli"

// Sky / invalid pixels report a viewZ beyond this so NRD ignores them. Near
// plane and the REBLUR hit-distance params must match the engine (0.1f and
// NrdDenoiser::kHitDistParams).
static const float kDenoisingRange = 1.0e6;
static const float kNearPlane = 0.1;
static const float3 kHitDistParams = float3(3.0, 0.1, 20.0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= pc.size.x || id.y >= pc.size.y) return;
  uint rng = (id.y * pc.size.x + id.x) * 9781u + pc.frame_index * 26699u + 1u;

  float2 ndc = (float2(id.xy) + 0.5) / float2(pc.size) * 2.0 - 1.0;
  float4 near_h = mul(pc.inv_view_proj, float4(ndc, 1.0, 1.0));
  float3 ro = pc.camera_pos.xyz;
  float3 dir = normalize(near_h.xyz / near_h.w - ro);
  float3 origin = ro;

  float3 sun = pc.sun_color.rgb * pc.sun_direction.w;
  float3 throughput = 1.0.xxx;
  float3 radiance = 0.0.xxx;    // lighting only (primary albedo demodulated)
  float3 background = 0.0.xxx;  // sky on a primary miss + primary emissive
  float3 prim_albedo = 1.0.xxx;
  float3 prim_normal = float3(0, 0, 1);
  float3 prim_pos = ro;
  bool primary_hit = false;
  float first_hit_dist = 1000.0;

  for (uint b = 0; b < pc.bounces; ++b) {
    Hit h = TraceClosest(origin, dir);
    if (b == 1) first_hit_dist = h.hit ? distance(h.position, prim_pos) : 1000.0;
    if (!h.hit) {
      if (b == 0) background += SampleSky(dir);
      else radiance += throughput * SampleSky(dir);
      break;
    }
    if (b == 0) {
      primary_hit = true;
      prim_pos = h.position;
      prim_normal = h.normal;
      prim_albedo = h.albedo;
      background += h.emissive;  // re-added after denoise, never demodulated
    } else {
      radiance += throughput * h.emissive;
    }

    float3 ldir = SunDirection(pc.sun_direction.xyz, pc.sun_color.w, rng);
    float ndl = dot(h.normal, ldir);
    if (ndl > 0.0 && !Occluded(h.position + h.normal * 0.002, ldir, 1000.0)) {
      radiance += throughput * h.albedo / kPi * sun * ndl;
    }

    dir = CosineHemisphere(h.normal, rng);
    origin = h.position + h.normal * 0.002;
    throughput *= h.albedo;
    if (max(throughput.r, max(throughput.g, throughput.b)) < 0.01) break;
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
    motion = (prev_ndc - ndc) * 0.5;  // uv-space delta, matches prepass.ps.hlsl
  }

  float norm_hit = REBLUR_FrontEnd_GetNormHitDist(first_hit_dist, viewz, kHitDistParams, 1.0);
  radiance_hitdist_out[id.xy] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(demod, norm_hit, false);
  normal_roughness_out[id.xy] = NRD_FrontEnd_PackNormalAndRoughness(prim_normal, 1.0, 0.0);
  viewz_out[id.xy] = viewz;
  motion_out[id.xy] = motion;
  albedo_out[id.xy] = float4(prim_albedo, 1.0);
  background_out[id.xy] = float4(background, primary_hit ? 1.0 : 0.0);
}
