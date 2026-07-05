#include "rhi_bindings.hlsli"
// Hair ribbon expansion. The index buffer walks (strand_block, point, side) with
// no vertex buffer; a strand block is one rendered strand. We simulate only the
// guide strands but render `children` clump children per guide: each child rides
// the same guide points, offset into a world-stable tuft that tapers to the tip,
// so the hair looks dense without simulating every strand.
struct HairPoint {
  float4 pos;
  float4 prev;
};
[[vk::binding(0, 0)]] StructuredBuffer<HairPoint> points : register(t0, space0);
[[vk::binding(1, 0)]] StructuredBuffer<float4> strand_color : register(t1, space0);  // per guide

struct DrawPush {
  column_major float4x4 view_proj;
  float4 camera;      // xyz eye, w = ribbon width
  float4 sun;         // xyz travel direction, w intensity
  float4 sun_color;   // rgb, w = clump radius
  float4 tint;        // rgb groom tint, w = children count
};
PUSH_CONSTANTS(DrawPush, pc);

static const uint kPointsPerStrand = 16;

struct VsOut {
  float4 pos : SV_Position;
  [[vk::location(0)]] float3 tangent : TANGENT;
  [[vk::location(1)]] float3 world_pos : POSITION1;
  [[vk::location(2)]] float along : TEXCOORD0;
  [[vk::location(3)]] float3 color : COLOR0;
};

float Hash(uint x) {
  x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
  return float(x) * (1.0 / 4294967296.0);
}

VsOut main(uint vid : SV_VertexID) {
  uint children = max(1u, uint(pc.tint.w));
  uint verts_per_strand = kPointsPerStrand * 2;
  uint block = vid / verts_per_strand;   // one rendered strand
  uint local = vid % verts_per_strand;
  uint pt = local / 2;
  float side = (local & 1) != 0 ? 1.0 : -1.0;
  uint guide = block / children;
  uint child = block % children;
  uint gbase = guide * kPointsPerStrand;

  float3 p = points[gbase + pt].pos.xyz;
  uint nxt = min(pt + 1, kPointsPerStrand - 1);
  uint prv = pt == 0 ? 0 : pt - 1;
  float3 tangent = normalize(points[gbase + nxt].pos.xyz - points[gbase + prv].pos.xyz + 1e-6);
  float along = float(pt) / float(kPointsPerStrand - 1);

  // World-stable clump offset: a per-child spiral in the plane perpendicular to
  // the strand, tapering to zero at the tip so children converge like a tuft.
  if (children > 1 && child > 0) {
    float3 up = abs(tangent.y) > 0.9 ? float3(1, 0, 0) : float3(0, 1, 0);
    float3 n1 = normalize(cross(tangent, up));
    float3 n2 = cross(tangent, n1);
    float frac = sqrt(float(child) / float(children - 1));  // fill the disc evenly
    float ang = float(child) * 2.399963 + Hash(guide) * 6.2831853;
    // Locks stay a rounded tube along the length (no hard tip convergence) and
    // each child adds a little tip frizz so a clump reads as strands, not a slab.
    float r = pc.sun_color.w * frac * (0.6 + 0.4 * along);
    float frizz = pc.sun_color.w * 0.5 * along * (Hash(block * 9u + 7u) - 0.5);
    p += (n1 * cos(ang) + n2 * sin(ang)) * r + n1 * frizz;
  }

  float3 view_dir = normalize(p - pc.camera.xyz);
  float3 right = normalize(cross(view_dir, tangent) + 1e-6);
  float width = pc.camera.w * (1.0 - 0.85 * along);
  float3 world = p + right * (side * width);

  float3 col = strand_color[guide].rgb * pc.tint.rgb;
  if (child > 0) col *= 0.82 + 0.36 * Hash(block);  // per-child shade variation

  VsOut o;
  o.pos = mul(pc.view_proj, float4(world, 1.0));
  o.tangent = tangent;
  o.world_pos = world;
  o.along = along;
  o.color = col;
  return o;
}
