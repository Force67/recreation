// Sky background: fullscreen triangle at depth 0 with EQUAL test so only
// untouched pixels shade. Writes camera-rotation motion vectors so temporal
// passes track the horizon.

#include "atmosphere.hlsli"

struct FrameGlobals {
  column_major float4x4 view_proj;
  column_major float4x4 prev_view_proj;
  column_major float4x4 inv_view_proj;
  float2 jitter;
  float2 prev_jitter;
  float4 sun_direction;
  float4 sun_color;
  float4 camera_position;
  float4 misc;  // x,y render size, z sun angular radius, w frame index
  uint flags;
  float time;  // seconds
  float2 pad;
};
[[vk::binding(0, 0)]] ConstantBuffer<FrameGlobals> frame : register(b0, space0);
static const uint kFrameFlagAurora = 256u;  // 1 << 8, mirrors mesh_pipeline.h

[[vk::combinedImageSampler]] [[vk::binding(0, 1)]] TextureCube sky : register(t0, space1);
[[vk::combinedImageSampler]] [[vk::binding(0, 1)]] SamplerState sky_sampler : register(s0, space1);
[[vk::combinedImageSampler]] [[vk::binding(1, 1)]] Texture2D<float4> transmittance_lut : register(t1, space1);
[[vk::combinedImageSampler]] [[vk::binding(1, 1)]] SamplerState transmittance_sampler : register(s1, space1);

struct PsOut {
  float4 color : SV_Target0;
  float2 motion : SV_Target1;
};

// Physical extinction toward the sun: the ground->sun transmittance straight
// from the Hillaire transmittance LUT, so the disc reddens and dims with the
// exact same optical depth as the scattered sky (no more air-mass fudge).
float3 SunExtinction(float sun_up) {
  float2 uv = TransmittanceUv(kGroundRadius + 500.0, sun_up);
  return transmittance_lut.SampleLevel(transmittance_sampler, uv, 0).rgb;
}

// A physically motivated sun: a limb-darkened disk (a 1 - u(1 - mu) law, redder
// at the limb because blue darkens more) at the correct angular size with a soft
// anti-aliased edge, wrapped in a two-lobe aureole so it reads as a radiant
// source the bloom pass flares on. All attenuated by atmospheric extinction, and
// added on top of the cubemap's scattered glow.
float3 SunDisk(float3 dir, float3 to_sun, float radius, float3 sun_color, float intensity) {
  radius = max(radius, 0.0008);
  if (to_sun.y < -0.05) return 0.0.xxx;  // below the horizon: no disk

  float mu = dot(dir, to_sun);
  float theta = acos(clamp(mu, -1.0, 1.0));            // angle from the sun centre
  float3 sun = sun_color * intensity * SunExtinction(to_sun.y);

  // Limb-darkened disk core, soft-edged over the last ~1.5% of the radius.
  float r = saturate(theta / radius);
  float mu_disk = sqrt(saturate(1.0 - r * r));         // cos of the heliocentric angle
  const float3 u = float3(0.42, 0.55, 0.68);           // limb-darkening, bluest darkest
  float3 limb = 1.0 - u * (1.0 - mu_disk);
  float edge = 1.0 - smoothstep(0.985, 1.0, r);
  const float kDiskBrightness = 220.0;                 // clips to white, drives the bloom
  float3 disk = sun * limb * edge * kDiskBrightness;

  // Aureole: a tight inner corona hugging the disk plus a broad outer halo, both
  // exponential in the angle past the rim. Gives the sun a luminous, punchy glow
  // without a hard ring.
  float past = max(theta - radius, 0.0);
  float inner = exp(-past / (radius * 2.5)) * 9.0;     // tight, bright corona (~1.3 deg)
  float outer = exp(-past / (radius * 14.0)) * 0.25;   // restrained wider halo
  float3 aureole = sun * (inner + outer);

  return disk + aureole;
}

// Orthonormal basis around a forward direction (for projecting a view ray onto
// the moon disc).
void Basis(float3 fwd, out float3 right, out float3 up) {
  float3 a = abs(fwd.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
  right = normalize(cross(a, fwd));
  up = cross(fwd, right);
}

float Hash21(float2 p) {
  p = frac(p * float2(123.34, 345.45));
  p += dot(p, p + 34.345);
  return frac(p.x * p.y);
}
float ValueNoise(float2 p) {
  float2 i = floor(p), f = frac(p);
  f = f * f * (3.0 - 2.0 * f);
  float a = Hash21(i), b = Hash21(i + float2(1, 0));
  float c = Hash21(i + float2(0, 1)), d = Hash21(i + float2(1, 1));
  return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}
float Fbm(float2 p) {
  float s = 0.0, a = 0.5;
  for (int i = 0; i < 4; ++i) { s += a * ValueNoise(p); p *= 2.0; a *= 0.5; }
  return s;
}

float Hash31(float3 p) {
  p = frac(p * 0.3183099 + 0.1);
  p *= 17.0;
  return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

// Procedural starfield fixed to the celestial sphere: a sparse set of jittered
// points on a directional grid, each a small bright dot with a colour tint.
// `fade` (1 at night, 0 by day) gates them out as the sun rises, so they only
// show against the dark sky.
float3 Stars(float3 dir, float fade) {
  if (dir.y < -0.02 || fade <= 0.0) return 0.0.xxx;
  float3 p = dir * 260.0;
  float3 cell = floor(p);
  float rnd = Hash31(cell);
  if (rnd < 0.965) return 0.0.xxx;  // ~3.5% of cells hold a star
  float3 jitter = float3(Hash31(cell + 1.3), Hash31(cell + 2.7), Hash31(cell + 5.1)) - 0.5;
  float d = length((frac(p) - 0.5) - jitter * 0.7);
  float core = saturate(1.0 - d * 7.0);
  core = core * core * core;
  float bright = (rnd - 0.965) / 0.035;  // 0..1 across the starred cells
  float3 tint = lerp(float3(0.75, 0.82, 1.0), float3(1.0, 0.9, 0.78), Hash31(cell + 9.0));
  return tint * core * (0.25 + bright * 1.6) * fade;
}

// A proper moon: a sphere sitting roughly anti-solar (so it rides high while the
// sun is down), shaded as a real body. The per-pixel surface normal is recovered
// from the disc, lit by the actual sun direction to give a correct phase
// terminator, textured with procedural maria/highlands, with earthshine on the
// dark limb, a soft anti-aliased rim and a faint halo. Brightness is reflected
// sunlight, so it only dominates against the dark night sky.
float3 Moon(float3 dir, float3 to_sun, float3 sun_color, float intensity) {
  float3 moon_dir = normalize(-to_sun + float3(0.50, 0.15, 0.0));  // off anti-solar -> gibbous
  if (moon_dir.y < -0.05) return 0.0.xxx;                          // below the horizon

  const float radius = 0.022;  // ~1.3 deg, large enough to read surface + phase
  float mu = dot(dir, moon_dir);
  float theta = acos(clamp(mu, -1.0, 1.0));

  // Faint cool halo so the moon reads as luminous against a dark sky.
  float glow = exp(-max(theta - radius, 0.0) / (radius * 2.5)) * 0.012;
  float3 result = float3(0.55, 0.62, 0.80) * glow;

  if (theta < radius) {
    float3 R, U;
    Basis(moon_dir, R, U);
    float2 disc = float2(dot(dir, R), dot(dir, U)) / sin(radius);  // [-1,1] across the disc
    float d2 = dot(disc, disc);
    if (d2 <= 1.0) {
      float z = sqrt(1.0 - d2);
      float3 n = normalize(R * disc.x + U * disc.y + moon_dir * z);  // surface normal
      // Phase: lit by the real sun, with a slightly soft terminator.
      float lit = saturate(dot(n, to_sun));
      lit = lit * smoothstep(0.0, 0.10, lit);
      // Procedural surface: dark maria vs brighter highlands, on a crude spherical uv.
      float2 suv = float2(atan2(n.x, n.z), asin(clamp(n.y, -1.0, 1.0)));
      float surf = Fbm(suv * 3.5 + 7.0);
      float3 albedo = lerp(float3(0.09, 0.09, 0.11), float3(0.46, 0.45, 0.41), surf);
      const float kEarthshine = 0.04;  // dark side faintly lit by earthlight
      float3 lambert = albedo * (lit + kEarthshine);
      float edge = 1.0 - smoothstep(0.985, 1.0, sqrt(d2));  // AA rim
      // Reflected sunlight: dim enough that night auto-exposure resolves the
      // phase + maria as a detailed disc rather than clipping to a white blob.
      const float kMoonBrightness = 0.12;
      result += lambert * sun_color * intensity * kMoonBrightness * edge;
    }
  }
  return result;
}

// Procedural aurora borealis (Skyrim's northern lights): undulating vertical
// curtains in a band of the night sky, green at the base fading to magenta tips,
// animated. Gated to night (fade) and to Skyrim (the aurora frame flag).
float3 Aurora(float3 dir, float fade, float time) {
  if (dir.y < 0.03 || fade <= 0.0) return 0.0.xxx;
  float alt = asin(saturate(dir.y));   // 0 horizon .. ~1.57 zenith
  float az = atan2(dir.x, dir.z);
  // The aurora lives in a mid-sky band (not the horizon, not the zenith).
  float band = smoothstep(0.05, 0.30, alt) * smoothstep(1.25, 0.5, alt);
  if (band <= 0.0) return 0.0.xxx;
  float t = time * 0.04;

  float glow = 0.0;
  for (int i = 0; i < 3; ++i) {
    float fi = float(i);
    // Low-frequency horizontal wave warps the curtain; high-frequency rays. A
    // coverage mask carves dark gaps between curtains so it isn't a solid wash.
    float warp = (Fbm(float2(az * (1.2 + fi * 0.5) + t * (0.6 + fi * 0.2), alt + fi)) - 0.5) * 2.5;
    float cover = smoothstep(0.45, 0.75, Fbm(float2(az * (2.5 + fi) + t * 0.5, fi * 7.0)));
    float ray = ValueNoise(
        float2(az * (26.0 + fi * 18.0) + warp + t * (1.4 + fi), alt * 3.0 + fi * 5.0));
    ray = pow(saturate(ray * 1.1 - 0.08), 5.0);   // thin, separated rays
    float vgrad = smoothstep(1.2, 0.12, alt);     // brighter lower in the curtain
    glow += ray * cover * vgrad * (0.5 - fi * 0.12);
  }
  glow *= band;
  float3 col = lerp(float3(0.15, 1.0, 0.45), float3(0.55, 0.25, 0.95), saturate((alt - 0.35) * 1.4));
  return col * glow * fade * 0.35;
}

PsOut main(float4 sv_position : SV_Position,
           [[vk::location(0)]] float2 uv : TEXCOORD0) {
  // Match the geometry jitter so temporal passes see a consistent frame.
  float2 ndc = uv * 2.0 - 1.0 + frame.jitter;
  float4 near = mul(frame.inv_view_proj, float4(ndc, 1.0, 1.0));  // reversed z near
  float3 dir = normalize(near.xyz / near.w - frame.camera_position.xyz);

  PsOut output;
  float3 col = sky.SampleLevel(sky_sampler, dir, 0).rgb;
  float3 to_sun = normalize(-frame.sun_direction.xyz);
  float night = smoothstep(0.04, -0.10, to_sun.y);  // 1 at night, 0 by day
  // Stars first (behind everything), fading out as the sun climbs.
  col += Stars(dir, night);
  // Aurora (Skyrim only, via the frame flag), behind the sun/moon.
  if ((frame.flags & kFrameFlagAurora) != 0u) col += Aurora(dir, night, frame.time);
  // Crisp limb-darkened sun disk on top of the cubemap's scattered glow.
  col += SunDisk(dir, to_sun, frame.misc.z, frame.sun_color.rgb, frame.sun_direction.w);
  // Phased moon, anti-solar so it rides high at night.
  col += Moon(dir, to_sun, frame.sun_color.rgb, frame.sun_direction.w);
  output.color = float4(col, 1.0);

  // Reproject a point far along the ray; translation is negligible there.
  float3 far_point = frame.camera_position.xyz + dir * 1e7;
  float4 curr = mul(frame.view_proj, float4(far_point, 1.0));
  float4 prev = mul(frame.prev_view_proj, float4(far_point, 1.0));
  output.motion = (prev.xy / prev.w - curr.xy / curr.w) * 0.5;
  return output;
}
