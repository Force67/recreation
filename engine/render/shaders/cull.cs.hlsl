// GPU frustum culling: one thread per draw instance tests its world bounding
// sphere against the camera frustum and writes the instanceCount (1 or 0) into
// every VkDrawIndexedIndirectCommand the instance owns. The opaque prepass and
// scene then issue those as indirect draws, so off-screen geometry costs no
// raster work. Disabled instances (skinned, no bounds) always pass.
struct Instance {
  column_major float4x4 model;
  float4 bounds;  // model-space sphere: xyz center, w radius
  uint first_cmd;
  uint cmd_count;
  uint cull_disabled;
  uint pad;
};
[[vk::binding(0, 0)]] StructuredBuffer<Instance> instances;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> commands;       // 5 u32 per draw command
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> visible_count;  // [0]

struct PushData {
  float4 planes[5];  // left, right, bottom, top, near (normalized, inside >= 0)
  uint instance_count;
  uint cull_enabled;
};
[[vk::push_constant]] PushData push;

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint i = id.x;
  if (i >= push.instance_count) return;
  Instance inst = instances[i];

  uint visible = 1;
  if (push.cull_enabled != 0u && inst.cull_disabled == 0u && inst.bounds.w > 0.0) {
    float3 center = mul(inst.model, float4(inst.bounds.xyz, 1.0)).xyz;
    float sx = length(inst.model[0].xyz);
    float sy = length(inst.model[1].xyz);
    float sz = length(inst.model[2].xyz);
    float radius = inst.bounds.w * max(sx, max(sy, sz));
    [unroll]
    for (int p = 0; p < 5; ++p) {
      if (dot(push.planes[p].xyz, center) + push.planes[p].w < -radius) {
        visible = 0;
        break;
      }
    }
  }

  for (uint k = 0; k < inst.cmd_count; ++k) {
    commands[(inst.first_cmd + k) * 5u + 1u] = visible;  // instanceCount field
  }
  if (visible != 0u) InterlockedAdd(visible_count[0], inst.cmd_count);
}
