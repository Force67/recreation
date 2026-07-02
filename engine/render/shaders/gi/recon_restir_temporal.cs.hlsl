#include "rhi_bindings.hlsli"
// ReSTIR GI, stage 1: temporal reservoir resampling (Ouyang et al. 2021).
// The gbuffer pass traced one cosine-sampled indirect path per pixel and
// stored its SAMPLE POINT (position, normal, outgoing radiance toward the
// primary hit). This pass runs streaming RIS: seed a reservoir with the fresh
// sample, then merge the motion-reprojected previous reservoir (validated by
// material id / viewZ / normal, M-clamped so stale samples age out). The
// spatial stage consumes the result; the SVGF chain cleans the residual noise.
//
// Measures: initial samples are drawn with pdf cos/pi in the solid angle of
// the CURRENT pixel, and temporal reprojection lands on (nearly) the same
// surface point, so no Jacobian is applied here (the spatial stage does).
struct ReconRestirTemporalPush {
  uint2 size;
  uint frame_index;
  float m_max;   // history length cap (in reservoir M)
  float reset;   // 1 = drop all history this frame
  float pad0;
  float pad1;
  float pad2;
};
PUSH_CONSTANTS(ReconRestirTemporalPush, pc);

// Reservoir layout across three textures (r = this frame, p = previous):
//   R0: xyz sample position, w reservoir weight W
//   R1: xyz sample normal (encoded *0.5+0.5), w M
//   R2: xyz sample radiance (outgoing, toward the visible point), w w_sum
[[vk::binding(0, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> r0_out : register(u0, space0);
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> r1_out : register(u1, space0);
[[vk::binding(2, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> r2_out : register(u2, space0);
[[vk::binding(3, 0)]] Texture2D<float4> s_pos : register(t3, space0);  // initial sample position (.w hit distance)
[[vk::binding(4, 0)]] Texture2D<float4> s_nrm : register(t4, space0);  // initial sample normal
[[vk::binding(5, 0)]] Texture2D<float4> s_rad : register(t5, space0);  // initial sample radiance
[[vk::binding(6, 0)]] Texture2D<float4> p_pos : register(t6, space0);  // primary hit world position (.w 0 = sky)
[[vk::binding(7, 0)]] Texture2D<float4> curr_nr : register(t7, space0);  // primary normal + roughness
[[vk::binding(8, 0)]] Texture2D<float4> prev_nr : register(t8, space0);
[[vk::binding(9, 0)]] Texture2D<float> curr_viewz : register(t9, space0);
[[vk::binding(10, 0)]] Texture2D<float> prev_viewz : register(t10, space0);
[[vk::binding(11, 0)]] Texture2D<uint> curr_matid : register(t11, space0);
[[vk::binding(12, 0)]] Texture2D<uint> prev_matid : register(t12, space0);
[[vk::binding(13, 0)]] Texture2D<float2> motion : register(t13, space0);
[[vk::binding(14, 0)]] Texture2D<float4> r0_prev : register(t14, space0);
[[vk::binding(15, 0)]] Texture2D<float4> r1_prev : register(t15, space0);
[[vk::binding(16, 0)]] Texture2D<float4> r2_prev : register(t16, space0);

static const float kPi = 3.14159265359;

uint Pcg(inout uint state) {
  state = state * 747796405u + 2891336453u;
  uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}
float Rand(inout uint state) { return (Pcg(state) & 0xffffffu) / 16777216.0; }

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float3 DecodeN(float4 nr) { return normalize(nr.xyz * 2.0 - 1.0); }
bool InBounds(int2 p) { return all(p >= 0) && all(p < int2(pc.size)); }

struct Reservoir {
  float3 pos;
  float3 nrm;
  float3 rad;
  float w_sum;
  float M;
  float W;
};

// Target function p_hat at a pixel with visible point vp / normal vn:
// luminance of the sample's radiance times the receiving cosine. The cosine
// keeps grazing samples from dominating; albedo is demodulated pipeline-wide.
float PHat(float3 vp, float3 vn, float3 sample_pos, float3 sample_rad) {
  float3 to_sample = sample_pos - vp;
  float len = length(to_sample);
  if (len < 1e-4) return 0.0;
  return Luma(sample_rad) * saturate(dot(vn, to_sample / len));
}

bool ValidateHistory(int2 cp, int2 pp) {
  if (!InBounds(pp)) return false;
  uint cm = curr_matid.Load(int3(cp, 0));
  uint pm = prev_matid.Load(int3(pp, 0));
  if (cm == 0xffffffffu || pm == 0xffffffffu || cm != pm) return false;
  float cz = curr_viewz.Load(int3(cp, 0));
  float pz = prev_viewz.Load(int3(pp, 0));
  if (abs(cz - pz) / max(cz, 1.0) > 0.05) return false;
  float3 cn = DecodeN(curr_nr.Load(int3(cp, 0)));
  float3 pn = DecodeN(prev_nr.Load(int3(pp, 0)));
  if (dot(cn, pn) < 0.9) return false;
  return true;
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  int2 p = int2(tid.xy);
  if (!InBounds(p)) return;

  float4 primary = p_pos.Load(int3(p, 0));
  if (primary.w == 0.0) {  // sky pixel: no visible surface, empty reservoir
    r0_out[p] = 0.0.xxxx;
    r1_out[p] = 0.0.xxxx;
    r2_out[p] = 0.0.xxxx;
    return;
  }
  float3 vp = primary.xyz;
  float3 vn = DecodeN(curr_nr.Load(int3(p, 0)));
  uint rng = (tid.y * pc.size.x + tid.x) * 15731u + pc.frame_index * 7919u + 3u;

  // Seed with this frame's sample: w = p_hat / pdf. The direction was drawn
  // cosine-weighted (pdf = cos/pi) and p_hat contains the same cosine, so the
  // ratio collapses to pi * luminance (0 for a null/absorbed sample).
  Reservoir r;
  r.pos = s_pos.Load(int3(p, 0)).xyz;
  r.nrm = DecodeN(s_nrm.Load(int3(p, 0)));
  r.rad = s_rad.Load(int3(p, 0)).rgb;
  r.w_sum = kPi * Luma(r.rad);
  r.M = 1.0;
  r.W = 0.0;

  // Temporal reuse: nearest reprojected reservoir (reservoirs cannot be
  // bilinearly blended), gated by the same surface tests SVGF uses.
  if (pc.reset == 0.0) {
    float2 uv = (float2(p) + 0.5) / float2(pc.size);
    float2 prev_uv = uv + motion.Load(int3(p, 0));
    int2 pp = int2(floor(prev_uv * float2(pc.size)));
    if (ValidateHistory(p, pp)) {
      float4 q0 = r0_prev.Load(int3(pp, 0));
      float4 q1 = r1_prev.Load(int3(pp, 0));
      float4 q2 = r2_prev.Load(int3(pp, 0));
      float qM = min(q1.w, pc.m_max);
      float qW = q0.w;
      if (qM > 0.0 && qW > 0.0) {
        float p_hat = PHat(vp, vn, q0.xyz, q2.xyz);
        float w = p_hat * qW * qM;
        if (w > 0.0 && !(w > 1.0e12)) {  // reject inf/nan history
          r.w_sum += w;
          if (Rand(rng) < w / r.w_sum) {
            r.pos = q0.xyz;
            r.nrm = DecodeN(q1);
            r.rad = q2.xyz;
          }
        }
        r.M += qM;
      }
    }
  }

  float p_hat_sel = PHat(vp, vn, r.pos, r.rad);
  r.W = p_hat_sel > 0.0 ? r.w_sum / (r.M * p_hat_sel) : 0.0;
  if (!(r.W < 1.0e12)) r.W = 0.0;  // NaN/inf guard: a dead reservoir, not a poisoned one

  r0_out[p] = float4(r.pos, r.W);
  r1_out[p] = float4(r.nrm * 0.5 + 0.5, r.M);
  r2_out[p] = float4(r.rad, r.w_sum);
}
