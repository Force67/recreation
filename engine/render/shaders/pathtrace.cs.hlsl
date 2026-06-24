// Progressive reference path tracer. Shares the scene's TLAS and bindless
// material/geometry tables with the realtime path (see pathtrace_common.hlsli);
// diffuse bounces with next-event estimation toward the sun and the procedural
// sky cube on miss. Accumulates one frame's samples into a persistent buffer
// that resets when the camera moves, so a still view converges to a ground-truth
// image. The denoised, playable variant lives in pathtrace_gbuffer.cs.hlsl.

struct PathPush {
  column_major float4x4 inv_view_proj;
  float4 camera_pos;     // xyz eye
  float4 sun_direction;  // xyz travel direction, w intensity
  float4 sun_color;      // rgb, w sun angular radius (radians)
  uint2 size;
  uint frame_index;
  uint sample_base;  // samples already accumulated (0 = overwrite)
  uint spp;          // samples this dispatch
  uint bounces;
  uint reset;
  uint pad;
};
[[vk::push_constant]] PathPush pc;

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> output_image;
[[vk::binding(1, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> accum_image;

#include "pathtrace_common.hlsli"

float3 Radiance(float3 origin, float3 dir, inout uint rng) {
  float3 throughput = 1.0.xxx;
  float3 radiance = 0.0.xxx;
  float3 sun = pc.sun_color.rgb * pc.sun_direction.w;
  for (uint b = 0; b < pc.bounces; ++b) {
    Hit h = TraceClosest(origin, dir);
    if (!h.hit) {
      radiance += throughput * SampleSky(dir);
      break;
    }
    radiance += throughput * h.emissive;

    // Next event estimation toward the (soft) sun disk.
    float3 ldir = SunDirection(pc.sun_direction.xyz, pc.sun_color.w, rng);
    float ndl = dot(h.normal, ldir);
    if (ndl > 0.0 && !Occluded(h.position + h.normal * 0.002, ldir, 1000.0)) {
      radiance += throughput * h.albedo / kPi * sun * ndl;
    }

    // Diffuse bounce; the cosine pdf cancels the albedo/pi * ndl factors.
    dir = CosineHemisphere(h.normal, rng);
    origin = h.position + h.normal * 0.002;
    throughput *= h.albedo;

    // Russian-roulette-free fixed depth; kill near-black paths early.
    if (max(throughput.r, max(throughput.g, throughput.b)) < 0.01) break;
  }
  return radiance;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= pc.size.x || id.y >= pc.size.y) return;
  uint rng = (id.y * pc.size.x + id.x) * 9781u + pc.frame_index * 26699u + 1u;

  float3 sum = 0.0.xxx;
  for (uint s = 0; s < pc.spp; ++s) {
    float2 jitter = float2(Rand(rng), Rand(rng));
    float2 ndc = (float2(id.xy) + jitter) / float2(pc.size) * 2.0 - 1.0;
    // Reversed infinite z: depth 1 is the near plane (finite w), depth 0 is at
    // infinity (w -> 0), so reconstruct the near point and aim from the eye.
    float4 near_h = mul(pc.inv_view_proj, float4(ndc, 1.0, 1.0));
    float3 p_near = near_h.xyz / near_h.w;
    float3 ro = pc.camera_pos.xyz;
    float3 rd = normalize(p_near - ro);
    sum += Radiance(ro, rd, rng);
  }

  float total = float(pc.sample_base + pc.spp);
  float3 accumulated = (pc.reset != 0u) ? sum : accum_image[id.xy].rgb + sum;
  accum_image[id.xy] = float4(accumulated, total);
  output_image[id.xy] = float4(accumulated / max(total, 1.0), 1.0);
}
