#include "rhi_bindings.hlsli"
// Volumetric clouds: a raymarched cloud layer (spherical shell) with procedural
// Perlin-ish density (coverage + erosion + a height gradient), Beer's-law
// self-shadowing toward the sun, a dual-lobe Henyey-Greenstein phase (silver
// lining), sky ambient, and depth-aware compositing so terrain occludes clouds.
// A simplified Nubis/Horizon-style model; procedural noise (no 3D textures).

#include "atmosphere.hlsli"

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> out_image : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D color_in : register(t1, space0);
[[vk::binding(2, 0)]] Texture2D depth_in : register(t2, space0);

struct PushData {
  column_major float4x4 inv_view_proj;
  float4 camera_pos;     // xyz eye (m), w time (s)
  float4 sun_direction;  // xyz travel direction, w intensity
  float4 sun_color;      // rgb, w coverage [0..1]
  float4 params;         // x bottom(m), y top(m), z density, w wind speed
  uint2 size;
  uint steps;
  uint light_steps;
};
PUSH_CONSTANTS(PushData, pc);

float Hash13(float3 p) {
  p = frac(p * 0.1031);
  p += dot(p, p.yzx + 33.33);
  return frac((p.x + p.y) * p.z);
}
float Noise3(float3 p) {
  float3 i = floor(p), f = frac(p);
  f = f * f * (3.0 - 2.0 * f);
  float n000 = Hash13(i), n100 = Hash13(i + float3(1, 0, 0));
  float n010 = Hash13(i + float3(0, 1, 0)), n110 = Hash13(i + float3(1, 1, 0));
  float n001 = Hash13(i + float3(0, 0, 1)), n101 = Hash13(i + float3(1, 0, 1));
  float n011 = Hash13(i + float3(0, 1, 1)), n111 = Hash13(i + float3(1, 1, 1));
  float nx00 = lerp(n000, n100, f.x), nx10 = lerp(n010, n110, f.x);
  float nx01 = lerp(n001, n101, f.x), nx11 = lerp(n011, n111, f.x);
  return lerp(lerp(nx00, nx10, f.y), lerp(nx01, nx11, f.y), f.z);
}
float Fbm3(float3 p, int octaves) {
  float s = 0.0, a = 0.5;
  for (int i = 0; i < octaves; ++i) {
    s += a * Noise3(p);
    p = p * 2.03 + 11.1;
    a *= 0.5;
  }
  return s;
}

// Density at a point (metres). 0 outside the slab.
float CloudDensity(float3 p, float time) {
  float r = length(p);
  float h = (r - (kGroundRadius + pc.params.x)) / (pc.params.y - pc.params.x);  // 0..1 in slab
  if (h <= 0.0 || h >= 1.0) return 0.0;
  // Rounded height gradient: bottom-weighted, fading to the top (cumulus-ish).
  float grad = saturate(h * 6.0) * saturate((1.0 - h) * 2.5);

  float3 wind = float3(time * pc.params.w, 0.0, time * pc.params.w * 0.3);
  float3 wp = (p + wind) * 0.00035;
  // Low-frequency domain warp: breaks the fbm's blobby isotropy into
  // billowing cauliflower masses.
  float warp = Fbm3(wp * 0.4 + 13.7, 2);
  wp += (warp - 0.5) * 0.9;
  float base = Fbm3(wp, 4);
  float coverage = pc.sun_color.w;
  float d = saturate(base - (1.0 - coverage)) * grad;
  if (d <= 0.0) return 0.0;
  // Erode the edges with higher-frequency detail.
  float erosion = Fbm3(wp * 4.0 + 5.0, 3);
  d = saturate(d - erosion * 0.4 * (1.0 - d));
  return d * pc.params.z;
}

// Beer's-law transmittance toward the sun (self-shadowing).
float LightMarch(float3 p, float3 to_sun, float time) {
  float dt = (pc.params.y - pc.params.x) / float(max(pc.light_steps, 1u)) * 0.6;
  float optical = 0.0;
  for (uint i = 0; i < pc.light_steps; ++i) {
    optical += CloudDensity(p + to_sun * ((float(i) + 0.5) * dt), time) * dt;
  }
  return exp(-optical * 0.04);
}

float HG(float c, float g) {
  float g2 = g * g;
  return (1.0 - g2) / (4.0 * kPi * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

float Ign(uint2 px, uint frame) {
  float3 m = float3(0.06711056, 0.00583715, 52.9829189);
  return frac(m.z * frac(dot(float2(px) + float(frame & 31u) * 5.588, m.xy)));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= pc.size.x || id.y >= pc.size.y) return;
  int2 px = int2(id.xy);
  float3 scene = color_in.Load(int3(px, 0)).rgb;
  float depth = depth_in.Load(int3(px, 0)).r;

  float2 ndc = (float2(px) + 0.5) / float2(pc.size) * 2.0 - 1.0;
  float4 nh = mul(pc.inv_view_proj, float4(ndc, 1.0, 1.0));  // reversed-z near
  float3 cam = pc.camera_pos.xyz;
  float3 view = normalize(nh.xyz / nh.w - cam);

  // Distance to the scene surface (sky pixels = infinite).
  float scene_dist = 1e12;
  if (depth > 0.0) {
    float4 wh = mul(pc.inv_view_proj, float4(ndc, depth, 1.0));
    scene_dist = length(wh.xyz / wh.w - cam);
  }

  // Cloud shell, in the atmosphere frame (+y up).
  float3 p0 = float3(0.0, kGroundRadius + max(cam.y, 0.0) + 1.0, 0.0);
  float Rb = kGroundRadius + pc.params.x;
  float Rt = kGroundRadius + pc.params.y;
  float rc = length(p0);
  float t_start, t_end;
  if (rc <= Rb) {
    t_start = RaySphere(p0, view, Rb);
    t_end = RaySphere(p0, view, Rt);
  } else if (rc >= Rt) {
    t_start = RaySphere(p0, view, Rt);
    float tb = RaySphere(p0, view, Rb);
    t_end = tb > 0.0 ? tb : t_start;
  } else {
    t_start = 0.0;
    t_end = RaySphere(p0, view, Rt);
    float tb = RaySphere(p0, view, Rb);
    if (tb > 0.0) t_end = min(t_end, tb);
  }
  t_end = min(t_end, scene_dist);
  if (t_start < 0.0 || t_end <= t_start || t_start > 150000.0) {
    out_image[px] = float4(scene, 1.0);
    return;
  }

  float3 to_sun = normalize(-pc.sun_direction.xyz);
  float3 sun_col = pc.sun_color.rgb * pc.sun_direction.w;
  // Cool sky ambient so shadowed cloud bottoms aren't black; graded by height
  // inside the march (tops see the whole sky dome, bases see the dark ground).
  float3 ambient_base = float3(0.50, 0.62, 0.88) * 0.38 * pc.sun_direction.w;
  float c = dot(view, to_sun);
  float phase = max(HG(c, 0.35), 0.6 * HG(c, -0.15));  // forward lobe + a back lobe

  uint steps = max(pc.steps, 1u);
  float dt = (t_end - t_start) / float(steps);
  float t = t_start + Ign(id.xy, asuint(pc.camera_pos.w)) * dt;  // per-pixel jitter
  float time = pc.camera_pos.w;

  float transmittance = 1.0;
  float3 scatter = 0.0.xxx;
  for (uint s = 0; s < steps; ++s) {
    float3 pos = p0 + view * t;
    float density = CloudDensity(pos, time);
    if (density > 0.001) {
      float light = LightMarch(pos, to_sun, time);
      // Beer-powder: multiple scattering darkens the crisp sun-facing edges
      // of dense cores before they saturate (the classic sugary cumulus
      // response, Schneider/Wrenninge).
      float powder = 1.0 - exp(-density * 14.0);
      float h = saturate((length(pos) - (kGroundRadius + pc.params.x)) /
                         max(pc.params.y - pc.params.x, 1.0));
      float3 ambient = ambient_base * (0.35 + 0.65 * h);
      float3 lit = sun_col * light * lerp(0.35, 1.0, powder) * phase + ambient;
      float sigma = density * 0.05;  // extinction per metre
      float step_trans = exp(-sigma * dt);
      // Energy-conserving front-to-back integration.
      scatter += transmittance * lit * density * (1.0 - step_trans);
      transmittance *= step_trans;
      if (transmittance < 0.01) break;
    }
    t += dt;
  }

  // High cirrus sheet: one sample of stretched 2d fbm on a shell above the
  // cumulus layer. Thin, wispy, catches the sun; nearly free and it fills the
  // empty upper sky.
  if (transmittance > 0.02) {
    float t_ci = RaySphere(p0, view, kGroundRadius + pc.params.y + 4200.0);
    if (t_ci > 0.0 && t_ci < min(scene_dist, 260000.0)) {
      float3 cp = p0 + view * t_ci;
      float2 cuv = cp.xz * 0.000045 + float2(time * pc.params.w * 0.00002, 0.0);
      float wisp = Fbm3(float3(cuv.x * 6.0, cuv.y * 1.4, 3.1), 4);  // wind-stretched
      float ci = saturate(wisp - 0.62) * 1.6 * saturate(pc.sun_color.w + 0.25);
      if (ci > 0.001) {
        float ci_phase = max(HG(c, 0.55), 0.4 * HG(c, -0.1));
        float3 ci_lit = sun_col * ci_phase * 0.9 + ambient_base * 0.8;
        scatter += transmittance * ci_lit * ci * 0.35;
        transmittance *= exp(-ci * 0.55);
      }
    }
  }

  float3 result = scene * transmittance + scatter;
  out_image[px] = float4(result, 1.0);
}
