// Screen-space sun shadow ray trace producing NRD's IN_PENUMBRA for the
// SIGMA_SHADOW denoiser. One ray per pixel toward the sun; the packed value
// carries the distance to the occluder so SIGMA can size the penumbra.
#include "NRD.hlsli"

[[vk::image_format("r16f")]] [[vk::binding(0, 0)]] RWTexture2D<float> penumbra_out;
[[vk::binding(1, 0)]] Texture2D<float> depth_map;
[[vk::binding(2, 0)]] RaytracingAccelerationStructure tlas;

struct PushData {
  column_major float4x4 inv_view_proj;  // unjittered
  float to_light_x;                     // direction toward the sun (normalized),
  float to_light_y;                     // kept as scalars to avoid float3 push
  float to_light_z;                     // constant alignment surprises
  float near_plane;
  float2 inv_size;
  float tan_angular_radius;             // tan(sun angular radius)
  float max_distance;                   // shadow ray length
};
[[vk::push_constant]] PushData push;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint width, height;
  penumbra_out.GetDimensions(width, height);
  if (id.x >= width || id.y >= height) return;
  int3 p = int3(id.xy, 0);

  float depth = depth_map.Load(p);
  if (depth <= 0.0) {  // sky: fully lit
    penumbra_out[id.xy] = SIGMA_FrontEnd_PackPenumbra(NRD_FP16_MAX, push.tan_angular_radius);
    return;
  }

  float2 uv = (float2(id.xy) + 0.5) * push.inv_size;
  float2 ndc = uv * 2.0 - 1.0;
  float4 world = mul(push.inv_view_proj, float4(ndc, depth, 1.0));
  float3 world_pos = world.xyz / world.w;

  float3 to_light = float3(push.to_light_x, push.to_light_y, push.to_light_z);
  RayDesc ray;
  ray.Origin = world_pos + to_light * 0.02;
  ray.TMin = 0.001;
  ray.Direction = to_light;
  ray.TMax = push.max_distance;
  RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> rq;
  rq.TraceRayInline(tlas, RAY_FLAG_NONE, 0xff, ray);
  rq.Proceed();

  float distance_to_occluder =
      rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? rq.CommittedRayT() : NRD_FP16_MAX;
  penumbra_out[id.xy] = SIGMA_FrontEnd_PackPenumbra(distance_to_occluder, push.tan_angular_radius);
}
