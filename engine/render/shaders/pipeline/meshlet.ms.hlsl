#include "rhi_bindings.hlsli"
// Mesh-shader meshlet path: one workgroup per meshlet. Each workgroup frustum-
// and backface-cone-culls its meshlet (cluster culling), and if visible emits
// the meshlet's vertices and triangles directly, no vertex fetch / index buffer.
// Vertices carry a flat meshlet id so the pixel shader can tint each cluster.
struct Meshlet {
  float4 center_radius;  // xyz bounding-sphere center, w radius
  float4 cone;           // xyz normal-cone axis, w cutoff (>=2 disables cone cull)
  uint vertex_offset;
  uint triangle_offset;
  uint vertex_count;
  uint triangle_count;
};
struct MVertex {
  float px, py, pz;  // scalar layout so the StructuredBuffer stride is an exact 24 bytes
  float nx, ny, nz;
};

[[vk::binding(0, 0)]] StructuredBuffer<Meshlet> meshlets : register(t0, space0);
[[vk::binding(1, 0)]] StructuredBuffer<uint> meshlet_vertices : register(t1, space0);  // index into vertices
[[vk::binding(2, 0)]] StructuredBuffer<uint> meshlet_triangles : register(t2, space0);  // 3 local indices packed per uint
[[vk::binding(3, 0)]] StructuredBuffer<MVertex> vertices : register(t3, space0);
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> visible_counter : register(u4, space0);  // [0] survivors this frame

struct PushData {
  column_major float4x4 view_proj;
  float4 planes[5];  // left,right,bottom,top,near (normalized, inside >= 0)
  float4 camera;     // xyz eye
};
PUSH_CONSTANTS(PushData, push);

struct VsOut {
  float4 pos : SV_Position;
  [[vk::location(0)]] float3 normal : NORMAL;
  [[vk::location(1)]] nointerpolation uint meshlet : MESHLET;
};

[outputtopology("triangle")]
[numthreads(128, 1, 1)]
void main(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex,
          out vertices VsOut verts[64], out indices uint3 tris[124]) {
  uint mi = gid.x;
  Meshlet m = meshlets[mi];

  // Cluster cull: frustum (bounds sphere vs planes) then backface normal cone.
  float3 c = m.center_radius.xyz;
  float r = m.center_radius.w;
  bool culled = false;
  [unroll]
  for (int p = 0; p < 5; ++p) {
    if (dot(push.planes[p].xyz, c) + push.planes[p].w < -r) culled = true;
  }
  float3 view_dir = normalize(c - push.camera.xyz);
  if (dot(view_dir, m.cone.xyz) >= m.cone.w) culled = true;
  if (culled) {
    SetMeshOutputCounts(0, 0);
    return;
  }

  SetMeshOutputCounts(m.vertex_count, m.triangle_count);
  if (tid == 0) InterlockedAdd(visible_counter[0], 1);

  for (uint v = tid; v < m.vertex_count; v += 128) {
    uint vid = meshlet_vertices[m.vertex_offset + v];
    MVertex mv = vertices[vid];
    VsOut o;
    o.pos = mul(push.view_proj, float4(mv.px, mv.py, mv.pz, 1.0));
    o.normal = float3(mv.nx, mv.ny, mv.nz);
    o.meshlet = mi;
    verts[v] = o;
  }
  for (uint t = tid; t < m.triangle_count; t += 128) {
    uint packed = meshlet_triangles[m.triangle_offset + t];
    tris[t] = uint3(packed & 0xff, (packed >> 8) & 0xff, (packed >> 16) & 0xff);
  }
}
