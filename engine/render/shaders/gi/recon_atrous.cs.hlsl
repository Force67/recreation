#include "rhi_bindings.hlsli"
// SVGF reconstruction, stage 3: edge-stopping a-trous wavelet filter. Run a few
// times with increasing step size (1,2,4,8). Each pass widens the spatial blur
// while edge-stopping on normal / depth / luminance (the luminance tolerance is
// driven by the temporal variance, so noisy regions blur more than clean ones).
struct ReconAtrousPush {
  uint2 size;
  uint step_size;
  float normal_phi;
  float depth_phi;
  float luma_phi;
  uint spec_mode;   // 1 = specular signal: tighten the kernel for smooth reflectors
  float spec_lobe;  // strength of the roughness-driven lobe tightening
};
PUSH_CONSTANTS(ReconAtrousPush, pc);

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> out_color : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float4> in_color : register(t1, space0);
[[vk::binding(2, 0)]] Texture2D<float4> in_nr : register(t2, space0);
[[vk::binding(3, 0)]] Texture2D<float> in_viewz : register(t3, space0);
[[vk::binding(4, 0)]] Texture2D<float4> in_moments : register(t4, space0);

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float3 DecodeN(float4 nr) { return normalize(nr.xyz * 2.0 - 1.0); }
bool InBounds(int2 p) { return all(p >= 0) && all(p < int2(pc.size)); }

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  int2 p = int2(tid.xy);
  if (!InBounds(p)) return;

  const float kernel[3] = {3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0};  // 5-tap b3 spline (|x|=0,1,2)

  float4 center4 = in_color.Load(int3(p, 0));
  float3 center_c = center4.rgb;
  float center_var = center4.a;  // variance travels in .a between passes
  // Defense in depth: a non-finite center would output NaN and spread as a black
  // rectangle. dot(c,c) < 1e30 is false for NaN and Inf.
  if (!(dot(center_c, center_c) < 1.0e30)) center_c = 0.0.xxx;
  if (!(center_var < 1.0e30)) center_var = 0.0;
  float4 center_nr = in_nr.Load(int3(p, 0));
  float3 center_n = DecodeN(center_nr);
  float center_rough = center_nr.a;
  float center_z = in_viewz.Load(int3(p, 0));
  float center_l = Luma(center_c);

  // SVGF: drive the luminance tolerance with a 3x3-prefiltered variance, not the
  // raw per-pixel value. At low history the per-pixel variance is itself noisy, so
  // using it directly makes the luma edge-stop randomly reject valid neighbors and
  // leaves blotches in freshly disoccluded regions. The prefilter step is always
  // 1px (independent of the a-trous step) so it estimates local noise, not extent.
  float gvar = center_var * 4.0;  // gaussian {1,2,1}/16 -> center weight 4
  float gwsum = 4.0;
  [unroll]
  for (int gy = -1; gy <= 1; ++gy) {
    [unroll]
    for (int gx = -1; gx <= 1; ++gx) {
      if (gx == 0 && gy == 0) continue;
      int2 gq = p + int2(gx, gy);
      if (!InBounds(gq)) continue;
      float gv = in_color.Load(int3(gq, 0)).a;
      if (!(gv < 1.0e30)) continue;
      float gk = (gx == 0 || gy == 0) ? 2.0 : 1.0;  // edge=2, corner=1
      gvar += gv * gk;
      gwsum += gk;
    }
  }
  gvar /= max(gwsum, 1e-6);
  // Variance-driven luminance tolerance: noisy pixels blur, clean ones stay sharp.
  float luma_denom = pc.luma_phi * sqrt(max(gvar, 0.0)) + 1e-4;

  // Sky / invalid pixels (viewZ at the denoising range) keep their value.
  if (center_z >= 1.0e6) {
    out_color[p] = float4(center_c, center_var);
    return;
  }

  float w0 = kernel[0] * kernel[0];
  float3 sum = center_c * w0;
  float sum_w = w0;
  float var_sum = center_var * w0 * w0;  // Var of a weighted mean uses w^2

  [unroll]
  for (int y = -2; y <= 2; ++y) {
    [unroll]
    for (int x = -2; x <= 2; ++x) {
      if (x == 0 && y == 0) continue;
      int2 q = p + int2(x, y) * int(pc.step_size);
      if (!InBounds(q)) continue;
      float qz = in_viewz.Load(int3(q, 0));
      if (qz >= 1.0e6) continue;

      float4 c4 = in_color.Load(int3(q, 0));
      float3 c = c4.rgb;
      if (!(dot(c, c) < 1.0e30)) continue;  // skip non-finite neighbors
      float qv = c4.a < 1.0e30 ? c4.a : 0.0;
      float3 n = DecodeN(in_nr.Load(int3(q, 0)));

      float wn = pow(saturate(dot(center_n, n)), pc.normal_phi);
      float wz = exp(-abs(center_z - qz) / max(center_z, 1.0) * pc.depth_phi);
      float wl = exp(-abs(center_l - Luma(c)) / luma_denom);
      float w = kernel[abs(x)] * kernel[abs(y)] * wn * wz * wl;

      // Specular: a sharp (low-roughness) reflection has a tight lobe and must not
      // be blurred into its neighbors. Attenuate far taps by the reflector's
      // roughness so mirrors stay crisp while rough surfaces filter normally
      // (roughness 1 -> factor 1, no change to the diffuse-equivalent behavior).
      if (pc.spec_mode != 0u) {
        float dist2 = float(x * x + y * y) * float(pc.step_size * pc.step_size);
        float smooth = 1.0 - center_rough;
        w *= exp(-dist2 * smooth * smooth * pc.spec_lobe);
      }

      sum += c * w;
      sum_w += w;
      var_sum += qv * w * w;
    }
  }

  float inv_w = 1.0 / max(sum_w, 1e-6);
  out_color[p] = float4(sum * inv_w, var_sum * inv_w * inv_w);  // filtered color + variance
}
