#include "rhi_bindings.hlsli"
// Hybrid-path ReSTIR DI, stage 1: per-pixel light-sample reservoirs
// (Bitterli et al. 2020) over the engine's clustered point/spot lights,
// running on the RASTER prepass G-buffer (depth + oct normal/roughness).
// Streaming RIS over K uniform candidate picks, then temporal reuse of the
// reprojected reservoir. No rays here; the spatial stage traces ONE shadow
// ray for the winner and shades. The sun keeps its SIGMA-denoised trace and
// area lights (types 2/3) stay analytic in the forward cluster loop - this
// pass owns exactly what the local shadow atlas used to approximate.
struct RestirDiTemporalPush {
  column_major float4x4 inv_view_proj;  // unjittered
  uint2 size;
  uint frame_index;
  uint light_count;
  uint candidates;  // uniform light picks per pixel
  float m_max;      // temporal age cap
  float reset;      // 1 = drop history
  float pad0;
};
PUSH_CONSTANTS(RestirDiTemporalPush, pc);

// Reservoir texel: x light id as float (-1 none, i = light buffer index),
// y w_sum, z M, w W.
[[vk::binding(0, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> r_out : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float> depth_map : register(t1, space0);
[[vk::binding(2, 0)]] Texture2D<float4> normal_map : register(t2, space0);  // oct rg, rough b
[[vk::binding(3, 0)]] Texture2D<float4> motion_map : register(t3, space0);  // uv, prev = uv + m
[[vk::binding(4, 0)]] Texture2D<float> prev_depth : register(t4, space0);
[[vk::binding(5, 0)]] Texture2D<float4> prev_normal : register(t5, space0);
[[vk::binding(6, 0)]] Texture2D<float4> r_prev : register(t6, space0);
struct PointLight {
  float4 pos_radius;       // xyz position, w influence radius
  float4 color_intensity;  // rgb color, w intensity
  float4 direction_type;   // xyz emit direction, w type (0 point 1 spot 2/3 area)
  float4 params;           // spot cos inner/outer; area extents; w shadow slot
};
[[vk::binding(7, 0)]] StructuredBuffer<PointLight> point_lights : register(t7, space0);

static const float kPi = 3.14159265;

uint Pcg(inout uint state) {
  state = state * 747796405u + 2891336453u;
  uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}
float Rand(inout uint state) { return (Pcg(state) & 0xffffffu) / 16777216.0; }
float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
bool InBounds(int2 p) { return all(p >= 0) && all(p < int2(pc.size)); }

float3 OctDecode(float2 o) {
  float3 d = float3(o.x, 1.0 - abs(o.x) - abs(o.y), o.y);
  if (d.y < 0.0) {
    float2 sign_xz = float2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
    d.xz = (1.0 - abs(d.zx)) * sign_xz;
  }
  return normalize(d);
}

// Diffuse-irradiance target, matching mesh.ps's windowed falloff exactly so
// the resampling distribution matches what the forward pass will shade.
float PHatLight(float3 x, float3 n, PointLight pl) {
  uint ltype = uint(pl.direction_type.w + 0.5);
  if (ltype > 1u) return 0.0;  // area lights stay analytic
  float3 to_l = pl.pos_radius.xyz - x;
  float dist2 = dot(to_l, to_l);
  float lr = pl.pos_radius.w;
  if (dist2 >= lr * lr) return 0.0;
  float dist = sqrt(max(dist2, 1e-8));
  float ndl = saturate(dot(n, to_l / dist));
  float falloff = saturate(1.0 - dist2 / (lr * lr));
  falloff *= falloff;
  if (ltype == 1u) {
    float cd = dot(-(to_l / dist), normalize(pl.direction_type.xyz));
    float att = saturate((cd - pl.params.y) / max(pl.params.x - pl.params.y, 1e-4));
    falloff *= att * att;
  }
  return Luma(pl.color_intensity.rgb) * pl.color_intensity.w * falloff * ndl;
}

float PHat(float3 x, float3 n, float light_id) {
  if (light_id < -0.5) return 0.0;
  uint id = (uint)round(light_id);
  if (id >= pc.light_count) return 0.0;  // light left the frame's buffer
  return PHatLight(x, n, point_lights[id]);
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  int2 p = int2(tid.xy);
  if (!InBounds(p)) return;

  float depth = depth_map.Load(int3(p, 0));
  if (depth <= 0.0 || pc.light_count == 0u) {  // sky, or nothing to sample
    r_out[p] = float4(-1.0, 0, 0, 0);
    return;
  }
  float2 uv = (float2(p) + 0.5) / float2(pc.size);
  float4 world = mul(pc.inv_view_proj, float4(uv * 2.0 - 1.0, depth, 1.0));
  float3 x = world.xyz / world.w;
  float3 n = OctDecode(normal_map.Load(int3(p, 0)).rg);

  uint rng = (tid.y * pc.size.x + tid.x) * 20749u + pc.frame_index * 12269u + 5u;

  // Streaming RIS: K uniform picks, candidate weight w = p_hat * N (pdf 1/N).
  float sel_id = -1.0;
  float w_sum = 0.0;
  float M = 0.0;
  float sel_phat = 0.0;
  for (uint c = 0; c < pc.candidates; ++c) {
    uint id = min(uint(Rand(rng) * pc.light_count), pc.light_count - 1u);
    float phat = PHatLight(x, n, point_lights[id]);
    float w = phat * float(pc.light_count);
    w_sum += w;
    M += 1.0;
    if (w > 0.0 && Rand(rng) * w_sum <= w) {
      sel_id = float(id);
      sel_phat = phat;
    }
  }

  // Temporal reuse: reproject, validate the surface, cap the age. The
  // history sample's target is re-evaluated at THIS pixel with THIS frame's
  // light buffer, so moving lights and geometry stay correct.
  if (pc.reset < 0.5) {
    float2 mv = motion_map.Load(int3(p, 0)).xy;
    int2 pp = int2((uv + mv) * float2(pc.size));
    if (InBounds(pp)) {
      float pz_here = depth_map.Load(int3(p, 0));
      float pz_prev = prev_depth.Load(int3(pp, 0));
      // Reversed-z raw compare with a loose relative window.
      bool depth_ok = abs(pz_prev - pz_here) <= 0.1 * max(pz_here, 1e-4);
      float3 pn = OctDecode(prev_normal.Load(int3(pp, 0)).rg);
      if (depth_ok && dot(n, pn) > 0.9) {
        float4 prev = r_prev.Load(int3(pp, 0));
        float prev_M = min(prev.z, pc.m_max);
        float phat_prev = PHat(x, n, prev.x);
        float w = phat_prev * prev.w * prev_M;
        w_sum += w;
        M += prev_M;
        if (w > 0.0 && Rand(rng) * w_sum <= w) {
          sel_id = prev.x;
          sel_phat = phat_prev;
        }
      }
    }
  }

  float W = (sel_phat > 0.0 && M > 0.0) ? w_sum / (sel_phat * M) : 0.0;
  r_out[p] = float4(sel_id, w_sum, M, W);
}
