#include "rhi_bindings.hlsli"
// ReSTIR GI, stage 2: spatial reservoir resampling + shading. Merges a few
// disk-distributed neighbor reservoirs into the pixel's temporal reservoir
// (geometry-gated, with the solid-angle Jacobian of Ouyang et al. 2021 Eq. 11
// applied through the shared sample point), traces ONE visibility ray to the
// winning sample, and emits the indirect diffuse irradiance:
//   E_indirect = L(sample -> visible) * cos(theta) * W
// The output adds the gbuffer's analytic direct term and feeds the SVGF
// temporal/a-trous chain unchanged. The visibility ray is force-opaque: the
// sample's radiance already alpha-tested its own path, this only guards reuse
// against leaking through walls (standard biased-variant tradeoff).
//
// The merged reservoir (post-visibility) is also written back as the frame's
// canonical reservoir: next frame's temporal stage reuses the SPATIALLY
// compounded history (the paper's feedback loop), which multiplies effective
// sample counts; the temporal M clamp keeps the correlation bounded.
struct ReconRestirSpatialPush {
  uint2 size;
  uint frame_index;
  uint sample_count;  // spatial neighbor taps
  float radius;       // neighbor disk radius, pixels
  uint debug;         // 0 off, 1 reservoir M / 60, 2 reservoir W / 5
  float pad1;
  float pad2;
};
PUSH_CONSTANTS(ReconRestirSpatialPush, pc);

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> irradiance_out : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float4> r0_in : register(t1, space0);  // reservoir: pos + W
[[vk::binding(2, 0)]] Texture2D<float4> r1_in : register(t2, space0);  // reservoir: normal + M
[[vk::binding(3, 0)]] Texture2D<float4> r2_in : register(t3, space0);  // reservoir: radiance + w_sum
[[vk::binding(4, 0)]] Texture2D<float4> p_pos : register(t4, space0);  // primary hit world position (.w 0 = sky)
[[vk::binding(5, 0)]] Texture2D<float4> curr_nr : register(t5, space0);  // primary normal + roughness
[[vk::binding(6, 0)]] Texture2D<float> curr_viewz : register(t6, space0);
[[vk::binding(7, 0)]] Texture2D<uint> curr_matid : register(t7, space0);
[[vk::binding(8, 0)]] Texture2D<float4> direct_irr : register(t8, space0);  // gbuffer's direct-only irradiance
[[vk::binding(9, 0)]] RaytracingAccelerationStructure tlas : register(t9, space0);
// Persistent reservoir history for next frame's temporal stage.
[[vk::binding(10, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> r0_out : register(u10, space0);
[[vk::binding(11, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> r1_out : register(u11, space0);
[[vk::binding(12, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> r2_out : register(u12, space0);

static const float kPi = 3.14159265359;
static const float kFarSample = 1.0e4;  // beyond this the sample is sky-like

uint Pcg(inout uint state) {
  state = state * 747796405u + 2891336453u;
  uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}
float Rand(inout uint state) { return (Pcg(state) & 0xffffffu) / 16777216.0; }

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float3 DecodeN(float4 nr) { return normalize(nr.xyz * 2.0 - 1.0); }
bool InBounds(int2 p) { return all(p >= 0) && all(p < int2(pc.size)); }

float PHat(float3 vp, float3 vn, float3 sample_pos, float3 sample_rad) {
  float3 to_sample = sample_pos - vp;
  float len = length(to_sample);
  if (len < 1e-4) return 0.0;
  return Luma(sample_rad) * saturate(dot(vn, to_sample / len));
}

bool Occluded(float3 origin, float3 dir, float dist) {
  RayDesc ray;
  ray.Origin = origin;
  ray.TMin = 0.001;
  ray.Direction = dir;
  ray.TMax = dist;
  RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> rq;
  rq.TraceRayInline(tlas, RAY_FLAG_NONE, 0xff, ray);
  rq.Proceed();
  return rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  int2 p = int2(tid.xy);
  if (!InBounds(p)) return;

  float3 direct = direct_irr.Load(int3(p, 0)).rgb;
  float4 primary = p_pos.Load(int3(p, 0));
  if (primary.w == 0.0) {  // sky: pass the (zero) direct term through
    irradiance_out[p] = float4(direct, 1.0);
    r0_out[p] = 0.0.xxxx;
    r1_out[p] = 0.0.xxxx;
    r2_out[p] = 0.0.xxxx;
    return;
  }
  float3 vp = primary.xyz;
  float3 vn = DecodeN(curr_nr.Load(int3(p, 0)));
  float vz = curr_viewz.Load(int3(p, 0));
  uint vid = curr_matid.Load(int3(p, 0));
  uint rng = (tid.y * pc.size.x + tid.x) * 26699u + pc.frame_index * 15731u + 11u;

  // Start from this pixel's temporal reservoir.
  float4 q0 = r0_in.Load(int3(p, 0));
  float4 q1 = r1_in.Load(int3(p, 0));
  float4 q2 = r2_in.Load(int3(p, 0));
  float3 sel_pos = q0.xyz;
  float3 sel_nrm = DecodeN(q1);
  float3 sel_rad = q2.xyz;
  float w_sum = q2.w;
  float M = q1.w;

  for (uint k = 0; k < pc.sample_count; ++k) {
    float ang = 2.0 * kPi * Rand(rng);
    float rad = pc.radius * sqrt(Rand(rng));
    int2 np = p + int2(round(float2(cos(ang), sin(ang)) * rad));
    if (!InBounds(np) || all(np == p)) continue;

    // Geometry gate: same object, similar depth and orientation, so the
    // neighbor's visible point sees a comparable hemisphere.
    if (curr_matid.Load(int3(np, 0)) != vid) continue;
    float nz = curr_viewz.Load(int3(np, 0));
    if (abs(nz - vz) / max(vz, 1.0) > 0.1) continue;
    float3 nn = DecodeN(curr_nr.Load(int3(np, 0)));
    if (dot(nn, vn) < 0.9) continue;
    float4 n_primary = p_pos.Load(int3(np, 0));
    if (n_primary.w == 0.0) continue;

    float4 n0 = r0_in.Load(int3(np, 0));
    float4 n1 = r1_in.Load(int3(np, 0));
    float4 n2 = r2_in.Load(int3(np, 0));
    float nM = n1.w;
    float nW = n0.w;
    if (nM <= 0.0 || nW <= 0.0) continue;

    // Solid-angle Jacobian through the shared sample point: the neighbor's W
    // is an inverse pdf in ITS solid angle; reusing at this pixel scales the
    // measure by (cos_q / cos_n) * (d_n^2 / d_q^2). Sky-distance samples are
    // measure-invariant between adjacent pixels.
    float3 s_p = n0.xyz;
    float3 s_n = DecodeN(n1);
    float3 vq = vp - s_p;
    float3 vn2 = n_primary.xyz - s_p;
    float d2q = dot(vq, vq);
    float d2n = dot(vn2, vn2);
    float jacobian = 1.0;
    if (d2q < kFarSample * kFarSample) {
      float cos_q = abs(dot(s_n, vq / max(sqrt(d2q), 1e-4)));
      float cos_n = abs(dot(s_n, vn2 / max(sqrt(d2n), 1e-4)));
      jacobian = (cos_q * d2n) / max(cos_n * d2q, 1e-8);
      // Extreme measure changes mean the reconnection geometry is degenerate;
      // rejecting beats importing a firefly.
      if (jacobian < 0.1 || jacobian > 10.0) continue;
    }

    float p_hat = PHat(vp, vn, s_p, n2.xyz);
    float w = p_hat * jacobian * nW * nM;
    if (!(w > 0.0) || w > 1.0e12) continue;
    w_sum += w;
    M += nM;
    if (Rand(rng) < w / w_sum) {
      sel_pos = s_p;
      sel_nrm = s_n;
      sel_rad = n2.xyz;
    }
  }

  float3 indirect = 0.0.xxx;
  float W = 0.0;
  float p_hat_sel = PHat(vp, vn, sel_pos, sel_rad);
  if (p_hat_sel > 0.0 && M > 0.0) {
    W = w_sum / (M * p_hat_sel);
    float3 to_sample = sel_pos - vp;
    float dist = length(to_sample);
    float3 dir = to_sample / dist;
    // One visibility ray for the winner: reused samples were traced from a
    // different visible point and may be behind local geometry here. A failed
    // test zeroes W in the history too: the temporal stage skips dead
    // reservoirs and reseeds, so occluded samples cannot linger.
    if (W > 0.0 && Occluded(vp + vn * 0.002, dir, min(dist - 0.004, 1000.0))) {
      W = 0.0;
    }
    indirect = sel_rad * saturate(dot(vn, dir)) * W;
  }
  if (!(W < 1.0e12)) W = 0.0;
  r0_out[p] = float4(sel_pos, W);
  r1_out[p] = float4(sel_nrm * 0.5 + 0.5, M);
  r2_out[p] = float4(sel_rad, w_sum);

  if (pc.debug == 1u) {
    irradiance_out[p] = float4((M / 60.0).xxx, 1.0);
    return;
  }
  if (pc.debug == 2u) {
    float W = (p_hat_sel > 0.0 && M > 0.0) ? w_sum / (M * p_hat_sel) : 0.0;
    irradiance_out[p] = float4((W / 5.0).xxx, 1.0);
    return;
  }

  // Sanitize like the gbuffer does: one NaN would smear across the a-trous.
  float3 e = direct + indirect;
  e.x = e.x >= 0.0 ? e.x : 0.0;
  e.y = e.y >= 0.0 ? e.y : 0.0;
  e.z = e.z >= 0.0 ? e.z : 0.0;
  e = min(e, 1.0e4.xxx);
  irradiance_out[p] = float4(e, 1.0);
}
