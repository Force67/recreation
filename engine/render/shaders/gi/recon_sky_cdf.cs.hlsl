#include "rhi_bindings.hlsli"
// Sky importance-sampling tables for ReSTIR DI: projects the sky cubemap onto
// a 64x32 equirectangular luminance grid and builds sampling CDFs. Cheap
// enough (2048 texels, one workgroup) to rebuild every frame, which keeps it
// correct through sunrise/sunset without sun-change tracking.
//
// Buffer layout (floats):
//   [0]                          total weight (sum of all cells)
//   [1 + r]                      marginal CDF over rows, r in [0,H)  (inclusive)
//   [1 + H + r*W + c]            per-row CDF over columns (inclusive)
//   [1 + H + W*H + r*W + c]      cell luminance (clamped like SampleSky)
// A cell's weight is luma * solid angle, so the direction pdf of a sample
// landing in cell c is simply luma_c / total (the solid-angle factors cancel).
// The DI temporal stage inverts the CDFs with binary searches.
static const uint kGridW = 128;
static const uint kGridH = 64;
struct ReconSkyCdfPush {
  uint2 grid;  // (kGridW, kGridH)
  uint pad0;
  uint pad1;
};
PUSH_CONSTANTS(ReconSkyCdfPush, pc);

[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] TextureCube sky_cube : register(t0, space0);
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState sky_sampler : register(s0, space0);
[[vk::binding(1, 0)]] RWStructuredBuffer<float> cdf : register(u1, space0);

static const float kPi = 3.14159265359;
// Matches SampleSky in recon_gbuffer / the DI shaders; the pdf must be built
// over the same clamped radiance the target function sees.
static const float kSkyClamp = 6.0;

groupshared float g_row_sum[64];

// Equirect cell -> world direction, y-up: v spans theta [0,pi] from +y,
// u spans phi [0,2pi).
float3 CellDir(float u, float v) {
  float theta = v * kPi;
  float phi = u * 2.0 * kPi;
  float s = sin(theta);
  return float3(s * cos(phi), cos(theta), s * sin(phi));
}

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint r = tid.x;
  uint w = pc.grid.x;   // 64 columns
  uint h = pc.grid.y;   // 32 rows
  if (r >= h) return;

  // Solid angle of one cell in row r: dphi * (cos(theta0) - cos(theta1)).
  float t0 = float(r) / float(h) * kPi;
  float t1 = float(r + 1u) / float(h) * kPi;
  float omega = (2.0 * kPi / float(w)) * (cos(t0) - cos(t1));

  float run = 0.0;
  for (uint c = 0; c < w; ++c) {
    float u = (float(c) + 0.5) / float(w);
    float v = (float(r) + 0.5) / float(h);
    float3 sky = min(sky_cube.SampleLevel(sky_sampler, CellDir(u, v), 0.0).rgb, kSkyClamp.xxx);
    float lum = dot(sky, float3(0.2126, 0.7152, 0.0722));
    cdf[1u + h + w * h + r * w + c] = lum;
    run += lum * omega;
    cdf[1u + h + r * w + c] = run;
  }
  g_row_sum[r] = run;
  GroupMemoryBarrierWithGroupSync();

  if (r == 0) {
    float total = 0.0;
    for (uint i = 0; i < h; ++i) {
      total += g_row_sum[i];
      cdf[1u + i] = total;
    }
    cdf[0] = total;
  }
}
