#include "rhi_bindings.hlsli"
// Hair ribbon expansion: no vertex buffer - the index buffer walks
// (strand, point, side) and the shader fetches the simulated point, expands
// it perpendicular to both the view ray and the strand tangent, and tapers
// the width toward the tip. vertex id = (point_index * 2 + side) within a
// strand block of points*2 vertices.
struct HairPoint {
  float4 pos;
  float4 prev;
};
[[vk::binding(0, 0)]] StructuredBuffer<HairPoint> points : register(t0, space0);

struct DrawPush {
  column_major float4x4 view_proj;
  float4 camera;      // xyz eye
  float4 sun;         // xyz travel direction, w intensity
  float4 sun_color;   // rgb, w unused
  uint points_per_strand;
  float width;
  float pad0;
  float pad1;
};
PUSH_CONSTANTS(DrawPush, pc);

struct VsOut {
  float4 pos : SV_Position;
  [[vk::location(0)]] float3 tangent : TANGENT;
  [[vk::location(1)]] float3 world_pos : POSITION1;
  [[vk::location(2)]] float along : TEXCOORD0;  // 0 root .. 1 tip
};

VsOut main(uint vid : SV_VertexID) {
  uint verts_per_strand = pc.points_per_strand * 2;
  uint strand = vid / verts_per_strand;
  uint local = vid % verts_per_strand;
  uint pt = local / 2;
  float side = (local & 1) != 0 ? 1.0 : -1.0;
  uint base = strand * pc.points_per_strand;

  float3 p = points[base + pt].pos.xyz;
  uint nxt = min(pt + 1, pc.points_per_strand - 1);
  uint prv = pt == 0 ? 0 : pt - 1;
  float3 tangent = normalize(points[base + nxt].pos.xyz - points[base + prv].pos.xyz + 1e-6);

  float along = float(pt) / float(pc.points_per_strand - 1);
  float3 view_dir = normalize(p - pc.camera.xyz);
  float3 right = normalize(cross(view_dir, tangent) + 1e-6);
  float width = pc.width * (1.0 - 0.7 * along);
  float3 world = p + right * (side * width);

  VsOut o;
  o.pos = mul(pc.view_proj, float4(world, 1.0));
  o.tangent = tangent;
  o.world_pos = world;
  o.along = along;
  return o;
}
