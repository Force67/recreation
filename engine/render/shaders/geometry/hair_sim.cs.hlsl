#include "rhi_bindings.hlsli"
// Strand simulation: one thread owns one strand (16 verlet points). Gravity +
// gusty wind integrate into the free points, a few Jakobsen iterations pull
// segments back to their rest lengths (root pinned), and a sphere keeps the
// hair outside the head. Cheap, stable, and entirely local per strand.
struct HairPoint {
  float4 pos;   // xyz position, w inv_mass (0 = pinned root)
  float4 prev;  // xyz previous position, w rest length to the previous point
};

[[vk::binding(0, 0)]] RWStructuredBuffer<HairPoint> points : register(u0, space0);

struct SimPush {
  float4 head;     // xyz center, w radius
  float4 wind;     // xyz base wind, w time
  uint strand_count;
  uint points_per_strand;
  float dt;
  float damping;
};
PUSH_CONSTANTS(SimPush, pc);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint strand = tid.x;
  if (strand >= pc.strand_count) return;
  uint base = strand * pc.points_per_strand;

  float dt = clamp(pc.dt, 0.001, 0.033);
  float3 gravity = float3(0.0, -9.8, 0.0);
  // Per-strand phase so the gusts ripple across the hair instead of moving
  // it as a rigid shell.
  float phase = float(strand) * 0.37;
  float3 wind = pc.wind.xyz * (0.6 + 0.4 * sin(pc.wind.w * 2.1 + phase)) +
                float3(0.0, 0.12 * sin(pc.wind.w * 3.7 + phase * 1.3), 0.0);

  // Verlet integration.
  HairPoint p[16];
  for (uint i = 0; i < pc.points_per_strand; ++i) {
    p[i] = points[base + i];
    if (p[i].pos.w > 0.0) {
      float3 vel = (p[i].pos.xyz - p[i].prev.xyz) * pc.damping;
      float3 next = p[i].pos.xyz + vel + (gravity + wind) * (dt * dt);
      p[i].prev.xyz = p[i].pos.xyz;
      p[i].pos.xyz = next;
    }
  }

  // Distance constraints + head collision.
  for (uint iter = 0; iter < 4; ++iter) {
    for (uint i = 1; i < pc.points_per_strand; ++i) {
      float3 d = p[i].pos.xyz - p[i - 1].pos.xyz;
      float len = max(length(d), 1e-6);
      float3 dir = d / len;
      float err = len - p[i].prev.w;  // rest length rides in prev.w
      float w0 = p[i - 1].pos.w, w1 = p[i].pos.w;
      float wsum = max(w0 + w1, 1e-6);
      p[i - 1].pos.xyz += dir * (err * (w0 / wsum));
      p[i].pos.xyz -= dir * (err * (w1 / wsum));
    }
    for (uint i = 0; i < pc.points_per_strand; ++i) {
      if (p[i].pos.w <= 0.0) continue;
      float3 to_p = p[i].pos.xyz - pc.head.xyz;
      float d = length(to_p);
      if (d < pc.head.w) p[i].pos.xyz = pc.head.xyz + to_p * (pc.head.w / max(d, 1e-6));
    }
  }

  for (uint i = 0; i < pc.points_per_strand; ++i) points[base + i] = p[i];
}
