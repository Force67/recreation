#include "rhi_bindings.hlsli"
// ReSTIR DI, stage 1: light-sample reservoirs (Bitterli et al. 2020) for the
// recon path tracer's PRIMARY direct lighting: the sun disk plus the engine's
// dynamic point lights (which the path-traced mode previously dropped).
// Streaming RIS over a discrete candidate set - the sun (deterministic
// proposal, with a fresh disk direction for the penumbra) and K uniform picks
// from the light buffer - then temporal reuse of the reprojected reservoir.
// No rays here; the spatial stage traces ONE alpha-tested shadow ray for the
// winner and shades. Targets are unshadowed contributions, demodulated
// (no albedo), matching the raster forward pass's windowed falloff.
//
// The SKY runs as a SEPARATE reservoir (B) fed by importance-sampled draws
// from the equirect luminance CDF built by recon_sky_cdf (pdf of a direction
// = cell luminance / total, solid-angle factors cancel). Keeping the sun and
// the sky out of one shared reservoir matters: a shared one turns every pixel
// into a sun-vs-sky lottery, and the SVGF temporal stage's 3x3 neighborhood
// clamp then eats the accumulated energy of whichever class lost locally this
// frame (measured -65% vs the reference). Two stable signals, summed after
// their own shadow rays, keep the clamp inert.
struct ReconRestirDiTemporalPush {
  float4 sun_direction;  // xyz travel direction, w intensity
  float4 sun_color;      // rgb, w sun angular radius (radians)
  uint2 size;
  uint frame_index;
  uint light_count;
  uint candidates;      // uniform light picks per pixel
  uint sky_candidates;  // sky CDF draws per pixel (0 = sky stays in the GI path)
  float m_max;          // reservoir A (sun + lights) age cap
  float reset;          // 1 = drop history
  float m_max_sky;      // reservoir B (sky) age cap
  float pad0;
  float pad1;
  float pad2;
};
PUSH_CONSTANTS(ReconRestirDiTemporalPush, pc);

// Reservoir layout (t = this frame's transient, p = previous persistent):
//   R0: xyz sun-sample disk direction; point lights derive their sample from
//       the id + the CURRENT light buffer, so moving lights stay correct.
//       w = light id as float (0 sun, 1+i point light i, -1 none)
//   R1: x w_sum, y M, z W
//   R2/R3: the same pair for the sky reservoir (R2.w: -1 none, -2 sky sample)
[[vk::binding(0, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> r0_out : register(u0, space0);
[[vk::binding(1, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> r1_out : register(u1, space0);
[[vk::binding(2, 0)]] Texture2D<float4> p_pos : register(t2, space0);    // primary pos (.w 0 = sky)
[[vk::binding(3, 0)]] Texture2D<float4> curr_nr : register(t3, space0);  // normal + roughness
[[vk::binding(4, 0)]] Texture2D<float4> prev_nr : register(t4, space0);
[[vk::binding(5, 0)]] Texture2D<float> curr_viewz : register(t5, space0);
[[vk::binding(6, 0)]] Texture2D<float> prev_viewz : register(t6, space0);
[[vk::binding(7, 0)]] Texture2D<uint> curr_matid : register(t7, space0);
[[vk::binding(8, 0)]] Texture2D<uint> prev_matid : register(t8, space0);
[[vk::binding(9, 0)]] Texture2D<float2> motion : register(t9, space0);
[[vk::binding(10, 0)]] Texture2D<float4> r0_prev : register(t10, space0);
[[vk::binding(11, 0)]] Texture2D<float4> r1_prev : register(t11, space0);
struct PointLight {
  float4 pos_radius;       // xyz position, w influence radius
  float4 color_intensity;  // rgb color, w intensity
  float4 direction_type;   // xyz emit direction, w type (0 point 1 spot 2/3 area)
  float4 params;           // spot cos inner/outer; area extents
};
[[vk::binding(12, 0)]] StructuredBuffer<PointLight> point_lights : register(t12, space0);
[[vk::combinedImageSampler]] [[vk::binding(13, 0)]] TextureCube sky_cube : register(t13, space0);
[[vk::combinedImageSampler]] [[vk::binding(13, 0)]] SamplerState sky_sampler : register(s13, space0);
[[vk::binding(14, 0)]] StructuredBuffer<float> sky_cdf : register(t14, space0);
[[vk::binding(15, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> r2_out : register(u15, space0);
[[vk::binding(16, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> r3_out : register(u16, space0);
[[vk::binding(17, 0)]] Texture2D<float4> r2_prev : register(t17, space0);
[[vk::binding(18, 0)]] Texture2D<float4> r3_prev : register(t18, space0);

static const float kPi = 3.14159265359;
static const float kSkyClamp = 6.0;  // matches SampleSky / recon_sky_cdf
static const uint kSkyGridW = 128;
static const uint kSkyGridH = 64;
static const uint kSkyRowCdf = 1u + kSkyGridH;
static const uint kSkyLuma = 1u + kSkyGridH + kSkyGridW * kSkyGridH;

uint Pcg(inout uint state) {
  state = state * 747796405u + 2891336453u;
  uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}
float Rand(inout uint state) { return (Pcg(state) & 0xffffffu) / 16777216.0; }

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float3 DecodeN(float4 nr) { return normalize(nr.xyz * 2.0 - 1.0); }
bool InBounds(int2 p) { return all(p >= 0) && all(p < int2(pc.size)); }

float3 SunDiskDir(inout uint rng) {
  float3 l = normalize(-pc.sun_direction.xyz);
  float radius = pc.sun_color.w;
  if (radius <= 0.0) return l;
  float3 up = abs(l.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
  float3 t1 = normalize(cross(up, l));
  float3 t2 = cross(l, t1);
  float a = 2.0 * kPi * Rand(rng);
  float r = sqrt(Rand(rng)) * radius;
  return normalize(l + t1 * (cos(a) * r) + t2 * (sin(a) * r));
}

float3 SampleSkyClamped(float3 dir) {
  return min(sky_cube.SampleLevel(sky_sampler, dir, 0.0).rgb, kSkyClamp.xxx);
}

// Table luminance of the cell containing dir (exact inverse of the CDF grid
// mapping). The sky target function p-hat reads THIS, not the cube: with the
// pdf built from the same table, candidate weights collapse to total*cos and
// stay bounded, so a bright-vs-dark radiance gradient inside one cell can
// never mint a huge-weight sample that robs the shared reservoir from the
// sun. Only the final shading (spatial stage) touches the true cube radiance;
// RIS stays unbiased for any positive target.
float SkyTableLuma(float3 dir) {
  float theta = acos(clamp(dir.y, -1.0, 1.0));
  float phi = atan2(dir.z, dir.x);
  if (phi < 0.0) phi += 2.0 * kPi;
  uint r = min(uint(theta / kPi * float(kSkyGridH)), kSkyGridH - 1u);
  uint c = min(uint(phi / (2.0 * kPi) * float(kSkyGridW)), kSkyGridW - 1u);
  return sky_cdf[kSkyLuma + r * kSkyGridW + c];
}

// Inverts the equirect CDFs (marginal rows, then the row's columns) into a
// direction + its solid-angle pdf. Jitters uniformly inside the chosen cell.
float3 SampleSkyCdf(inout uint rng, out float pdf) {
  pdf = 0.0;
  float total = sky_cdf[0];
  if (!(total > 0.0)) return float3(0, 1, 0);
  float target_r = Rand(rng) * total;
  uint lo = 0, hi = kSkyGridH - 1;
  while (lo < hi) {
    uint mid = (lo + hi) >> 1;
    if (sky_cdf[1u + mid] < target_r) lo = mid + 1; else hi = mid;
  }
  uint row = lo;
  float row_above = row > 0 ? sky_cdf[1u + row - 1u] : 0.0;
  float row_sum = sky_cdf[1u + row] - row_above;
  if (!(row_sum > 0.0)) return float3(0, 1, 0);
  // Column search runs on the raw (luma-only) per-row cumsum; the row's
  // constant solid-angle factor cancels out of the conditional.
  float row_cum_last = sky_cdf[kSkyRowCdf + row * kSkyGridW + kSkyGridW - 1u];
  float target_c = Rand(rng) * row_cum_last;
  lo = 0; hi = kSkyGridW - 1;
  while (lo < hi) {
    uint mid = (lo + hi) >> 1;
    if (sky_cdf[kSkyRowCdf + row * kSkyGridW + mid] < target_c) lo = mid + 1; else hi = mid;
  }
  uint col = lo;
  float lum = sky_cdf[kSkyLuma + row * kSkyGridW + col];
  if (!(lum > 0.0)) return float3(0, 1, 0);
  pdf = lum / total;  // cell solid angle cancels (weights are luma * omega)
  float u = (float(col) + Rand(rng)) / float(kSkyGridW);
  float v = (float(row) + Rand(rng)) / float(kSkyGridH);
  float theta = v * kPi;
  float phi = u * 2.0 * kPi;
  float sn = sin(theta);
  return float3(sn * cos(phi), cos(theta), sn * sin(phi));
}

// Unshadowed target function, demodulated irradiance luminance. Shared with
// the spatial stage; any change must stay in sync.
float PHatSun(float3 n, float3 disk_dir) {
  return Luma(pc.sun_color.rgb) * pc.sun_direction.w * saturate(dot(n, disk_dir));
}
float PHatLight(float3 x, float3 n, PointLight pl) {
  float3 to_l = pl.pos_radius.xyz - x;
  float dist2 = dot(to_l, to_l);
  float lr = pl.pos_radius.w;
  if (dist2 >= lr * lr) return 0.0;
  float dist = sqrt(max(dist2, 1e-8));
  float ndl = saturate(dot(n, to_l / dist));
  // Radius-windowed falloff, matching mesh.ps exactly.
  float falloff = saturate(1.0 - dist2 / (lr * lr));
  falloff *= falloff;
  if (pl.direction_type.w >= 0.5 && pl.direction_type.w < 1.5) {  // spot
    float cd = dot(-(to_l / dist), normalize(pl.direction_type.xyz));
    float att = saturate((cd - pl.params.y) / max(pl.params.x - pl.params.y, 1e-4));
    falloff *= att * att;
  }
  return Luma(pl.color_intensity.rgb) * pl.color_intensity.w * falloff * ndl;
}
float PHatSky(float3 n, float3 dir) {
  float ndl = saturate(dot(n, dir));
  if (ndl <= 0.0) return 0.0;
  return SkyTableLuma(dir) * ndl;
}
float PHat(float3 x, float3 n, float light_id, float3 dir) {
  if (light_id < -0.5) return 0.0;  // -1 = empty reservoir
  uint id = (uint)round(light_id);
  if (id == 0u) return PHatSun(n, dir);
  if (id - 1u >= pc.light_count) return 0.0;  // light left the frame's buffer
  return PHatLight(x, n, point_lights[id - 1u]);
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
  if (primary.w == 0.0) {  // sky
    r0_out[p] = float4(0, 0, 0, -1.0);
    r1_out[p] = 0.0.xxxx;
    r2_out[p] = float4(0, 0, 0, -1.0);
    r3_out[p] = 0.0.xxxx;
    return;
  }
  float3 x = primary.xyz;
  float3 n = DecodeN(curr_nr.Load(int3(p, 0)));
  uint rng = (tid.y * pc.size.x + tid.x) * 20749u + pc.frame_index * 12269u + 5u;

  // Streaming RIS over the candidate set. Candidate weight w = p_hat / pdf:
  // the sun is a deterministic proposal (pdf 1); each of the K uniform picks
  // has pdf 1/N, so w = p_hat * N. The 1/M in W averages the strategies.
  float sel_id = -1.0;
  float3 sel_dir = 0.0.xxx;
  float w_sum = 0.0;
  float M = 0.0;

  float3 sun_dir = SunDiskDir(rng);
  {
    float w = PHatSun(n, sun_dir);
    w_sum += w;
    M += 1.0;
    if (w > 0.0) {  // first candidate: always selected while it is the only mass
      sel_id = 0.0;
      sel_dir = sun_dir;
    }
  }
  for (uint k = 0; k < pc.candidates && pc.light_count > 0u; ++k) {
    uint li = min(uint(Rand(rng) * float(pc.light_count)), pc.light_count - 1u);
    float w = PHatLight(x, n, point_lights[li]) * float(pc.light_count);
    M += 1.0;
    if (!(w > 0.0)) continue;
    w_sum += w;
    if (Rand(rng) < w / w_sum) {
      sel_id = float(li + 1u);
      sel_dir = 0.0.xxx;
    }
  }

  // Temporal reuse: nearest reprojected reservoir, revalidated against the
  // CURRENT light buffer (ids re-resolve, so moving lights stay correct and
  // vanished lights age out through a zero target).
  if (pc.reset == 0.0) {
    float2 uv = (float2(p) + 0.5) / float2(pc.size);
    float2 prev_uv = uv + motion.Load(int3(p, 0));
    int2 pp = int2(floor(prev_uv * float2(pc.size)));
    if (ValidateHistory(p, pp)) {
      float4 q0 = r0_prev.Load(int3(pp, 0));
      float4 q1 = r1_prev.Load(int3(pp, 0));
      float qM = min(q1.y, pc.m_max);
      float qW = q1.z;
      if (qM > 0.0 && qW > 0.0 && q0.w > -0.5) {
        float p_hat = PHat(x, n, q0.w, q0.xyz);
        float w = p_hat * qW * qM;
        if (w > 0.0 && !(w > 1.0e12)) {
          w_sum += w;
          if (Rand(rng) < w / w_sum) {
            sel_id = q0.w;
            sel_dir = q0.xyz;
          }
        }
        M += qM;
      }
    }
  }

  float p_hat_sel = PHat(x, n, sel_id, sel_dir);
  float W = (p_hat_sel > 0.0 && M > 0.0) ? w_sum / (M * p_hat_sel) : 0.0;
  if (!(W < 1.0e12)) W = 0.0;

  r0_out[p] = float4(sel_dir, sel_id);
  r1_out[p] = float4(w_sum, M, W, 0.0);

  // --- Reservoir B: the sky. Candidate weights are bounded (w = p-hat/pdf =
  // total * cos, both read the CDF table), reuse mirrors reservoir A. ---
  float sky_id = -1.0;
  float3 sky_dir = 0.0.xxx;
  float sky_w_sum = 0.0;
  float sky_M = 0.0;
  for (uint q = 0; q < pc.sky_candidates; ++q) {
    float pdf;
    float3 dir = SampleSkyCdf(rng, pdf);
    sky_M += 1.0;
    if (!(pdf > 0.0)) continue;
    float w = PHatSky(n, dir) / pdf;
    if (!(w > 0.0) || w > 1.0e12) continue;
    sky_w_sum += w;
    if (Rand(rng) < w / sky_w_sum) {
      sky_id = -2.0;
      sky_dir = dir;
    }
  }
  if (pc.reset == 0.0 && pc.sky_candidates > 0u) {
    float2 uv = (float2(p) + 0.5) / float2(pc.size);
    float2 prev_uv = uv + motion.Load(int3(p, 0)).xy;
    int2 pp = int2(floor(prev_uv * float2(pc.size)));
    if (ValidateHistory(p, pp)) {
      float4 q2 = r2_prev.Load(int3(pp, 0));
      float4 q3 = r3_prev.Load(int3(pp, 0));
      float qM = min(q3.y, pc.m_max_sky);
      float qW = q3.z;
      if (qM > 0.0 && qW > 0.0 && q2.w < -1.5) {
        float p_hat = PHatSky(n, q2.xyz);
        float w = p_hat * qW * qM;
        if (w > 0.0 && !(w > 1.0e12)) {
          sky_w_sum += w;
          if (Rand(rng) < w / sky_w_sum) {
            sky_id = -2.0;
            sky_dir = q2.xyz;
          }
        }
        sky_M += qM;
      }
    }
  }
  float p_hat_sky = sky_id < -1.5 ? PHatSky(n, sky_dir) : 0.0;
  float sky_W = (p_hat_sky > 0.0 && sky_M > 0.0) ? sky_w_sum / (sky_M * p_hat_sky) : 0.0;
  if (!(sky_W < 1.0e12)) sky_W = 0.0;

  r2_out[p] = float4(sky_dir, sky_id);
  r3_out[p] = float4(sky_w_sum, sky_M, sky_W, 0.0);
}
