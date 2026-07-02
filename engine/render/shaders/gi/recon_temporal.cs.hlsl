#include "rhi_bindings.hlsli"
// SVGF reconstruction, stage 2: temporal accumulation. Reproject last frame's
// accumulated irradiance through the motion vector, reject stale history by
// material id / viewZ / normal, clamp to the current 3x3 neighborhood, and blend.
// Also tracks luminance moments -> variance + history length for the atrous pass.
struct ReconTemporalPush {
  column_major float4x4 prev_view_proj;  // spec: virtual-point reprojection
  float4 camera_pos;                     // xyz eye
  uint2 size;
  float2 inv_size;
  float current_weight_min;  // floor on the current-frame weight (responsiveness)
  float max_history;         // history length cap (frames)
  float reset;               // 1 = drop all history this frame
  uint spec_mode;            // 1 = specular signal: virtual-point reprojection
};
PUSH_CONSTANTS(ReconTemporalPush, pc);

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> out_accum : register(u0, space0);
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> out_moments : register(u1, space0);
[[vk::binding(2, 0)]] Texture2D<float4> curr_noisy : register(t2, space0);
[[vk::binding(3, 0)]] Texture2D<float4> prev_accum : register(t3, space0);
[[vk::binding(4, 0)]] Texture2D<float4> curr_nr : register(t4, space0);
[[vk::binding(5, 0)]] Texture2D<float4> prev_nr : register(t5, space0);
[[vk::binding(6, 0)]] Texture2D<float> curr_viewz : register(t6, space0);
[[vk::binding(7, 0)]] Texture2D<float> prev_viewz : register(t7, space0);
[[vk::binding(8, 0)]] Texture2D<float2> motion : register(t8, space0);
[[vk::binding(9, 0)]] Texture2D<uint> curr_matid : register(t9, space0);
[[vk::binding(10, 0)]] Texture2D<uint> prev_matid : register(t10, space0);
[[vk::binding(11, 0)]] Texture2D<float4> prev_moments : register(t11, space0);
[[vk::binding(12, 0)]] Texture2D<float4> primary_pos : register(t12, space0);  // .w 0 = sky

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float3 DecodeN(float4 nr) { return normalize(nr.xyz * 2.0 - 1.0); }
bool InBounds(int2 p) { return all(p >= 0) && all(p < int2(pc.size)); }

bool ValidateHistory(int2 cp, int2 pp) {
  if (!InBounds(pp)) return false;
  uint cm = curr_matid.Load(int3(cp, 0));
  uint pm = prev_matid.Load(int3(pp, 0));
  if (cm == 0xffffffffu || pm == 0xffffffffu || cm != pm) return false;
  float cz = curr_viewz.Load(int3(cp, 0));
  float pz = prev_viewz.Load(int3(pp, 0));
  // Specular virtual reprojection lands on a DIFFERENT point of the same
  // reflector, so its depth naturally differs; matid + normal carry the test.
  float z_tolerance = pc.spec_mode != 0u ? 0.2 : 0.03;
  if (abs(cz - pz) / max(cz, 1.0) > z_tolerance) return false;
  float3 cn = DecodeN(curr_nr.Load(int3(cp, 0)));
  float3 pn = DecodeN(prev_nr.Load(int3(pp, 0)));
  if (dot(cn, pn) < 0.9) return false;
  return true;
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  int2 p = int2(tid.xy);
  if (!InBounds(p)) return;

  float3 curr = curr_noisy.Load(int3(p, 0)).rgb;

  float2 uv = (float2(p) + 0.5) * pc.inv_size;
  float2 prev_uv = uv + motion.Load(int3(p, 0));

  // Specular: a reflection's image moves with its VIRTUAL point (the sample
  // mirrored behind the reflector at primary distance + reflection hit
  // distance), not with the surface. Reproject that point with last frame's
  // camera and blend toward plain surface motion as roughness widens the lobe
  // (a rough "reflection" is diffuse-like and tracks the surface).
  if (pc.spec_mode != 0u) {
    float4 prim = primary_pos.Load(int3(p, 0));
    float hit_t = curr_noisy.Load(int3(p, 0)).a;  // gbuffer packs reflection hit distance
    if (prim.w != 0.0 && hit_t > 0.0) {
      float3 view_vec = prim.xyz - pc.camera_pos.xyz;
      float view_d = max(length(view_vec), 1e-4);
      float3 virtual_pos = pc.camera_pos.xyz + view_vec / view_d * (view_d + hit_t);
      float4 clip = mul(pc.prev_view_proj, float4(virtual_pos, 1.0));
      if (clip.w > 1e-4) {
        float2 virtual_uv = (clip.xy / clip.w) * 0.5 + 0.5;  // engine convention, no y-flip
        float rough = curr_nr.Load(int3(p, 0)).a;
        prev_uv = lerp(virtual_uv, prev_uv, smoothstep(0.15, 0.5, rough));
      }
    }
  }

  // Bilinear reprojection: gather the 2x2 prev texels around the (sub-pixel)
  // history sample, weighting by the bilinear fraction AND per-tap validity, so
  // sub-pixel camera pans track smoothly instead of snapping to integer texels.
  // floor()+per-tap InBounds also drops off-screen taps cleanly (no truncate-to-0
  // aliasing for slightly-negative coords).
  float2 prev_pix = prev_uv * float2(pc.size) - 0.5;
  int2 base = int2(floor(prev_pix));
  float2 fr = prev_pix - float2(base);
  float bw[4] = {(1.0 - fr.x) * (1.0 - fr.y), fr.x * (1.0 - fr.y), (1.0 - fr.x) * fr.y,
                 fr.x * fr.y};
  const int2 off[4] = {int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1)};

  float3 prev_c = 0.0.xxx;
  float2 prev_m = 0.0.xx;
  float prev_len = 0.0;
  float wsum = 0.0;
  if (pc.reset == 0.0) {
    [unroll]
    for (uint k = 0; k < 4; ++k) {
      int2 pp = base + off[k];
      if (bw[k] <= 0.0 || !ValidateHistory(p, pp)) continue;
      float w = bw[k];
      prev_c += w * prev_accum.Load(int3(pp, 0)).rgb;
      float4 pm = prev_moments.Load(int3(pp, 0));
      prev_m += w * pm.xy;
      prev_len += w * pm.w;
      wsum += w;
    }
  }
  bool valid = wsum > 0.0;
  if (valid) {
    float inv = 1.0 / wsum;
    prev_c *= inv;
    prev_m *= inv;
    prev_len *= inv;
  }

  // Neighborhood clamp of the reprojected history to the current 3x3 range
  // (kills ghosting where reprojection is imperfect), and accumulate the 3x3
  // luminance moments for the spatial variance estimate used below.
  float3 mn = curr, mx = curr;
  float s_l = 0.0, s_l2 = 0.0;
  [unroll]
  for (int y = -1; y <= 1; ++y)
    [unroll]
    for (int x = -1; x <= 1; ++x) {
      float3 c = curr_noisy.Load(int3(clamp(p + int2(x, y), int2(0, 0), int2(pc.size) - 1), 0)).rgb;
      mn = min(mn, c);
      mx = max(mx, c);
      float cl = Luma(c);
      s_l += cl;
      s_l2 += cl * cl;
    }
  float3 ext = (mx - mn) * 0.5;
  prev_c = clamp(prev_c, mn - ext, mx + ext);

  float len = valid ? min(prev_len + 1.0, pc.max_history) : 1.0;
  float weight = valid ? max(1.0 / len, pc.current_weight_min) : 1.0;

  float3 accum = lerp(prev_c, curr, weight);

  float l = Luma(curr);
  float2 cm = float2(l, l * l);
  float2 m = valid ? lerp(prev_m, cm, weight) : cm;
  float temporal_var = max(0.0, m.y - m.x * m.x);
  // SVGF: temporal variance is unreliable until a few frames have accumulated,
  // so for low history use a spatial 3x3 luminance variance estimate instead.
  float spatial_var = max(0.0, s_l2 / 9.0 - (s_l / 9.0) * (s_l / 9.0));
  float variance = len < 4.0 ? spatial_var : temporal_var;

  out_accum[p] = float4(accum, variance);  // .a carries variance to the a-trous
  out_moments[p] = float4(m.x, m.y, variance, len);
}
