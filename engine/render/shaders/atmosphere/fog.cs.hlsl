#include "rhi_bindings.hlsli"
// Ray-marched volumetric fog: exponential height fog with single-scattering
// from the sun, shadowed per step through the scene TLAS so it produces real
// light shafts (god rays). Marches from the eye to the depth-buffer surface,
// jittered per pixel and per frame so the temporal pass resolves the noise.

struct FogPush {
  column_major float4x4 inv_view_proj;
  float4 camera_pos;     // xyz eye
  float4 sun_direction;  // xyz travel direction, w intensity
  float4 sun_color;      // rgb, w anisotropy g
  float4 params;         // x density, y height falloff, z base height, w max distance
  uint2 size;
  uint steps;
  uint frame_index;
};
PUSH_CONSTANTS(FogPush, pc);

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> output_image : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D color_in : register(t1, space0);  // full res, fetched per texel
[[vk::binding(2, 0)]] Texture2D depth_in : register(t2, space0);  // raw reversed-z depth export
[[vk::binding(3, 0)]] RaytracingAccelerationStructure tlas : register(t3, space0);

static const float kPi = 3.14159265359;

float Ign(float2 p, uint frame) {
  p += float(frame & 63u) * 5.588238;  // golden-ratio frame decorrelation
  return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// Henyey-Greenstein phase function.
float Phase(float cos_theta, float g) {
  float g2 = g * g;
  float d = 1.0 + g2 - 2.0 * g * cos_theta;
  return (1.0 - g2) / (4.0 * kPi * max(pow(d, 1.5), 1e-4));
}

float Density(float3 p) {
  // Exponential height falloff above a base plane.
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
  float3 scene = color_in.Load(int3(id.xy, 0)).rgb;

  // Reconstruct the view ray and the surface distance from reversed-z depth.
  float2 ndc = uv * 2.0 - 1.0;
  float4 near_h = mul(pc.inv_view_proj, float4(ndc, 1.0, 1.0));
  float3 p_near = near_h.xyz / near_h.w;
  float3 ro = pc.camera_pos.xyz;
  float3 rd = normalize(p_near - ro);

  float depth = depth_in.Load(int3(id.xy, 0)).r;
  float march_end = pc.params.w;  // sky: fog to the far cap
  if (depth > 1e-6) {
    float4 world_h = mul(pc.inv_view_proj, float4(ndc, depth, 1.0));
    float3 world = world_h.xyz / world_h.w;
    march_end = min(length(world - ro), pc.params.w);
  }

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
    // Sun single scattering, shadowed by the scene for true light shafts.
    float visible = SunOccluded(p, to_sun) ? 0.0 : 1.0;
    float3 scattered = sun * visible * phase;
    inscatter += transmittance * (1.0 - step_t) * scattered;
    transmittance *= step_t;
    if (transmittance < 0.01) break;
  }

  float3 result = scene * transmittance + inscatter;
  output_image[id.xy] = float4(result, 1.0);
}
