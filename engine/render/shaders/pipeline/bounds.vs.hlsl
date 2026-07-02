#include "rhi_bindings.hlsli"
// Acceleration-structure / cull-bounds debug view: draws a wireframe box around
// each opaque instance's world bounding sphere (the volume the gpu cull and the
// tlas instances use). Vertex-pulled from the same cull instance buffer, twelve
// line-list edges per box.
struct Instance {
  column_major float4x4 model;
  float4 bounds;  // model-space sphere: xyz center, w radius
  uint first_cmd;
  uint cmd_count;
  uint cull_disabled;
  uint pad;
};
[[vk::binding(0, 0)]] StructuredBuffer<Instance> instances : register(t0, space0);

struct PushData {
  column_major float4x4 view_proj;
};
PUSH_CONSTANTS(PushData, push);

// Corner index per line endpoint: 12 edges of a cube = 24 vertices.
static const uint kEdges[24] = {0, 1, 1, 3, 3, 2, 2, 0, 4, 5, 5, 7,
                                7, 6, 6, 4, 0, 4, 1, 5, 2, 6, 3, 7};

float4 main(uint vid : SV_VertexID, uint iid : SV_InstanceID) : SV_Position {
  Instance inst = instances[iid];
  if (inst.bounds.w <= 0.0) return float4(2, 2, 2, 1);  // unbounded: skip

  float3 center = mul(inst.model, float4(inst.bounds.xyz, 1.0)).xyz;
  float sx = length(inst.model[0].xyz);
  float sy = length(inst.model[1].xyz);
  float sz = length(inst.model[2].xyz);
  float radius = inst.bounds.w * max(sx, max(sy, sz));

  uint c = kEdges[vid];
  float3 sign = float3((c & 1u) ? 1.0 : -1.0, (c & 2u) ? 1.0 : -1.0, (c & 4u) ? 1.0 : -1.0);
  return mul(push.view_proj, float4(center + radius * sign, 1.0));
}
