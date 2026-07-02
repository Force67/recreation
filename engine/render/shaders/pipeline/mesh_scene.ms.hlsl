#include "rhi_bindings.hlsli"
// Optional mesh-shader opaque path (VK_EXT_mesh_shader). The task stage
// (mesh_scene.as) culls instances and meshlets and hands this stage a compacted
// list of surviving meshlet indices; one mesh workgroup per survivor pulls its
// vertices through buffer device addresses and emits the exact interpolants the
// raster vertex shader (mesh.vs) does, so mesh.ps / mesh_rt.ps / prepass.ps
// shade the result unchanged. Meshlet / vertex / triangle layouts mirror
// meshlet.h and asset::Vertex.

struct FrameGlobals {
  column_major float4x4 view_proj;
  column_major float4x4 prev_view_proj;
  column_major float4x4 inv_view_proj;
  float2 jitter;
  float2 prev_jitter;
  float4 sun_direction;
  float4 sun_color;
  float4 camera_position;  // xyz eye
  float4 misc;
  uint flags;
  float3 pad;
};
[[vk::binding(0, 0)]] ConstantBuffer<FrameGlobals> frame : register(b0, space0);

struct PushData {
  column_major float4x4 model;
  column_major float4x4 prev_model;
  float4 bounds;                    // instance bounds (used by the task stage)
  float4 occlusion;                 // hi-z cull params (used by the task stage)
  uint64_t meshlets_addr;           // array of 48-byte Meshlet
  uint64_t meshlet_vertices_addr;   // array of uint (global vertex index)
  uint64_t meshlet_triangles_addr;  // array of uint (3 local indices packed)
  uint64_t vertices_addr;           // array of 52-byte asset::Vertex
  uint meshlet_offset;              // first meshlet of this (lod,submesh)
  uint meshlet_count;              // meshlet count of this (lod,submesh)
};
PUSH_CONSTANTS(PushData, push);

struct MeshPayload {
  uint meshlet[32];  // survivor meshlet indices from the task stage
};

// Must match mesh.vs / mesh.ps / prepass.ps.
struct VsOut {
  float4 sv_position : SV_Position;
  [[vk::location(0)]] float3 normal : NORMAL;
  [[vk::location(1)]] float4 curr_clip : TEXCOORD1;
  [[vk::location(2)]] float4 prev_clip : TEXCOORD2;
  [[vk::location(3)]] float3 world_pos : TEXCOORD3;
  [[vk::location(4)]] float4 tangent : TANGENT;
  [[vk::location(5)]] float2 uv : TEXCOORD0;
  [[vk::location(6)]] float4 color : COLOR0;
};

static const uint kMeshletStride = 48;
static const uint kVertexStride = 52;

float LoadF(uint64_t base, uint off) { return vk::RawBufferLoad<float>(base + off); }
uint LoadU(uint64_t base, uint off) { return vk::RawBufferLoad<uint>(base + off); }

[outputtopology("triangle")]
[numthreads(128, 1, 1)]
void main(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex, in payload MeshPayload pl,
          out vertices VsOut verts[64], out indices uint3 tris[124]) {
  uint mi = pl.meshlet[gid.x];  // task stage already culled; this is a survivor
  uint64_t mbase = push.meshlets_addr + (uint64_t)mi * kMeshletStride;
  uint vertex_offset = LoadU(mbase, 32);
  uint triangle_offset = LoadU(mbase, 36);
  uint vertex_count = LoadU(mbase, 40);
  uint triangle_count = LoadU(mbase, 44);

  SetMeshOutputCounts(vertex_count, triangle_count);

  for (uint v = tid; v < vertex_count; v += 128) {
    uint vid = LoadU(push.meshlet_vertices_addr, (vertex_offset + v) * 4);
    uint64_t vb = push.vertices_addr + (uint64_t)vid * kVertexStride;
    float3 pos = float3(LoadF(vb, 0), LoadF(vb, 4), LoadF(vb, 8));
    float3 nrm = float3(LoadF(vb, 12), LoadF(vb, 16), LoadF(vb, 20));
    float4 tan = float4(LoadF(vb, 24), LoadF(vb, 28), LoadF(vb, 32), LoadF(vb, 36));
    float2 uv = float2(LoadF(vb, 40), LoadF(vb, 44));
    uint cpk = LoadU(vb, 48);  // R8G8B8A8 unorm

    float4 world = mul(push.model, float4(pos, 1.0));
    float4 clip = mul(frame.view_proj, world);
    VsOut o;
    o.world_pos = world.xyz;
    o.curr_clip = clip;
    o.prev_clip = mul(frame.prev_view_proj, mul(push.prev_model, float4(pos, 1.0)));
    o.sv_position = clip + float4(frame.jitter * clip.w, 0.0, 0.0);
    o.normal = mul((float3x3)push.model, nrm);
    o.tangent = float4(mul((float3x3)push.model, tan.xyz), tan.w);
    o.uv = uv;
    o.color = float4(cpk & 0xff, (cpk >> 8) & 0xff, (cpk >> 16) & 0xff, (cpk >> 24) & 0xff) / 255.0;
    verts[v] = o;
  }
  for (uint t = tid; t < triangle_count; t += 128) {
    uint packed = LoadU(push.meshlet_triangles_addr, (triangle_offset + t) * 4);
    tris[t] = uint3(packed & 0xff, (packed >> 8) & 0xff, (packed >> 16) & 0xff);
  }
}
