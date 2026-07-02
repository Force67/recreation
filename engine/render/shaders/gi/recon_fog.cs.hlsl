#include "rhi_bindings.hlsli"
// Volumetric fog for the recon path tracer: exponential height fog with sun
// single-scattering, shadowed per step through the TLAS (light shafts), the
// same density/phase model as the raster fog pass so toggling path tracing
// does not change the fog's look. Marches eye -> primary hit (from the
// gbuffer's world-position target), jittered per pixel/frame, and resolves
// its own noise with a motion-reprojected EMA (the recon pipeline runs
// without TAA, so unlike the raster pass nothing downstream would absorb the
// jitter). The composite applies scene * transmittance + inscatter.
struct ReconFogPush {
  column_major float4x4 inv_view_proj;
  float4 camera_pos;     // xyz eye
  float4 sun_direction;  // xyz travel direction, w intensity
  float4 sun_color;      // rgb, w anisotropy g
  float4 params;         // x density, y height falloff, z base height, w max distance
  uint2 size;       // fog target resolution (half the render res)
  uint steps;
  uint frame_index;
  float reset;           // 1 = drop history
  uint2 full_size;       // gbuffer resolution for the p_pos/motion reads
  float pad0;
};
PUSH_CONSTANTS(ReconFogPush, pc);

// Output/history: rgb in-scattered radiance, a transmittance.
[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> fog_out : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float4> fog_prev : register(t1, space0);
[[vk::binding(2, 0)]] Texture2D<float4> p_pos : register(t2, space0);   // primary pos (.w 0 = sky)
[[vk::binding(3, 0)]] Texture2D<float2> motion : register(t3, space0);  // uv current -> previous
[[vk::binding(4, 0)]] RaytracingAccelerationStructure tlas : register(t4, space0);

static const float kPi = 3.14159265359;
static const float kBlend = 0.12;  // EMA weight of the current frame

float Ign(float2 p, uint frame) {
  p += float(frame & 63u) * 5.588238;  // golden-ratio frame decorrelation
  return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// Henyey-Greenstein phase function (matches fog.cs).
float Phase(float cos_theta, float g) {
  float g2 = g * g;
  float d = 1.0 + g2 - 2.0 * g * cos_theta;
  return (1.0 - g2) / (4.0 * kPi * max(pow(d, 1.5), 1e-4));
}

float Density(float3 p) {
  return pc.params.x * exp(-max(0.0, p.y - pc.params.z) * pc.params.y);
}

bool SunOccluded(float3 origin, float3 dir) {
  RayDesc ray;
  ray.Origin = origin;
  ray.TMin = 0.02;
  ray.Direction = dir;
  ray.TMax = 1000.0;
  RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> rq;
  rq.TraceRayInline(tlas, RAY_FLAG_NONE, 0xff, ray);
  rq.Proceed();
  return rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= pc.size.x || id.y >= pc.size.y) return;
  float2 uv = (float2(id.xy) + 0.5) / float2(pc.size);
  // Fog marches at half res (it is low frequency; the composite upsamples
  // bilinearly); gbuffer reads map to the matching full-res texel.
  int2 fp = min(int2(id.xy) * 2, int2(pc.full_size) - 1);

  float2 ndc = uv * 2.0 - 1.0;
  float4 near_h = mul(pc.inv_view_proj, float4(ndc, 1.0, 1.0));
  float3 ro = pc.camera_pos.xyz;
  float3 rd = normalize(near_h.xyz / near_h.w - ro);

  float4 primary = p_pos.Load(int3(fp, 0));
  float march_end = pc.params.w;  // sky: fog to the far cap
  if (primary.w != 0.0) march_end = min(length(primary.xyz - ro), pc.params.w);

  float3 to_sun = normalize(-pc.sun_direction.xyz);
  float3 sun = pc.sun_color.rgb * pc.sun_direction.w;
  float phase = Phase(dot(rd, to_sun), pc.sun_color.w);

  uint steps = max(pc.steps, 1u);
  float dt = march_end / float(steps);
  float jitter = Ign(float2(id.xy), pc.frame_index);
  float3 inscatter = 0.0.xxx;
  float transmittance = 1.0;
  for (uint i = 0; i < steps; ++i) {
    float t = (float(i) + jitter) * dt;
    float3 p = ro + rd * t;
    float density = Density(p) * dt;
    if (density < 1e-5) continue;
    float step_t = exp(-density);
    float visible = SunOccluded(p, to_sun) ? 0.0 : 1.0;
    inscatter += transmittance * (1.0 - step_t) * (sun * visible * phase);
    transmittance *= step_t;
    if (transmittance < 0.01) break;
  }
  float4 current = float4(inscatter, transmittance);

  // Temporal EMA against the reprojected history. Validation keys off the
  // TRANSMITTANCE channel: it only carries jitter noise (no binary shadow
  // term), so a big change means a real disocclusion (surface distance or
  // density along the ray changed) and the history is dropped. The noisy
  // in-scatter channel itself is never clamped - with binary per-step sun
  // visibility a window around one jittered sample would trap bright shafts
  // near zero on occluded-jitter frames.
  if (pc.reset == 0.0) {
    float2 prev_uv = uv + motion.Load(int3(fp, 0)).xy;
    int2 pp = int2(floor(prev_uv * float2(pc.size)));
    if (all(pp >= 0) && all(pp < int2(pc.size))) {
      float4 prev = fog_prev.Load(int3(pp, 0));
      if (abs(prev.a - current.a) < 0.25) {
        current = lerp(prev, current, kBlend);
      }
    }
  }
  fog_out[id.xy] = current;
}
