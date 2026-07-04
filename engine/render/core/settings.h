#ifndef RECREATION_RENDER_SETTINGS_H_
#define RECREATION_RENDER_SETTINGS_H_

#include "core/math.h"
#include "core/types.h"
#include "render/post/antialiasing.h"
#include "render/post/upscaler.h"

namespace rec::render {

enum class TonemapOperator : u8 { kAces, kReinhard, kNone, kAgx };

// Built-in display-space color grades, baked into a strip LUT and applied after
// tonemapping. kNeutral is identity (the LUT is skipped entirely).
enum class ColorGrade : u8 { kNeutral, kWarm, kCool, kCinematic, kCustom };

// Debug visualization of an isolated shading channel, written straight to the
// scene target (still tonemapped). kOff is the normal lit image. Mirrored in
// mesh.ps.hlsl as the debug_view field of FrameGlobals.
enum class DebugView : u8 {
  kOff,
  kBaseColor,
  kWorldNormal,
  kRoughness,
  kMetallic,
  kAmbientOcclusion,
  kIndirectGi,
  kDirectLight,
  kEmissive,
  kReflection,  // raw traced specular reflection (rt variant only)
  kOverdraw,    // additive geometry heat ramp (own pass, not a shading channel)
  kBounds,      // cull / acceleration-structure bounding boxes (overlay)
  kTemporalHistory,  // taa disocclusion heatmap (where history is rejected)
  kMotionVectors,    // screen-space velocity (red = +x, green = +y, grey = still)
  kRayCount,         // per-pixel ray-tracing cost heatmap (shadow + ao + reflection)
  kLightComplexity,  // per-pixel count of dynamic point lights affecting it
};

// Resolution scaling presets matching the vendor upscaler naming. The ratio
// is per axis: render = output / ratio.
enum class UpscalerQuality : u8 { kNativeAa, kQuality, kBalanced, kPerformance };

inline f32 UpscalerScale(UpscalerQuality quality) {
  switch (quality) {
    case UpscalerQuality::kNativeAa: return 1.0f;
    case UpscalerQuality::kQuality: return 1.5f;
    case UpscalerQuality::kBalanced: return 1.7f;
    case UpscalerQuality::kPerformance: return 2.0f;
  }
  return 1.5f;
}

// Everything the debug ui can flip at runtime. The renderer diffs this
// against the applied state each frame and reconfigures what changed;
// expensive transitions (upscaler swaps, vsync) go through a device idle.
struct RenderSettings {
  AntiAliasingMode aa_mode = AntiAliasingMode::kTaa;
  UpscalerKind upscaler = UpscalerKind::kNone;
  UpscalerQuality upscaler_quality = UpscalerQuality::kQuality;
  f32 sharpness = 0.0f;  // 0..1, used by upscalers that sharpen
  f32 taa_history_blend = 0.9f;
  // Internal render resolution as a fraction of output, when no upscaler is
  // active. >1 supersamples (renders above the window, the post pass downscales);
  // <1 renders cheaper. Ignored while an upscaler drives the resolution.
  f32 render_scale = 1.0f;

  bool rt_shadows = true;  // masked by device caps and the renderer desc
  f32 sun_angular_radius = 0.005f;  // radians; 0 reverts to hard shadows

  // Cascaded shadow maps: the raster sun-shadow path. Used when ray-traced
  // shadows are unavailable or off, so non-rt tiers still cast sun shadows.
  bool shadow_maps = false;
  u32 shadow_resolution = 2048;  // per-cascade square; presets drop it on handhelds
  f32 shadow_distance = 160.0f;  // furthest shadowed camera distance, meters
  bool wireframe = false;
  bool vsync = false;
  bool gpu_culling = true;  // gpu compute frustum culling of the opaque indirect draws
  bool gpu_occlusion = true;  // hi-z occlusion culling against last frame's depth
  bool distance_lod = false;  // pick coarser mesh lods by distance; off = always finest (it's 2026)
  bool mesh_shader_lod = false;  // optional VK_EXT_mesh_shader opaque path (per-meshlet gpu cull)
  DebugView debug_view = DebugView::kOff;  // isolate a shading channel

  bool sky = true;  // procedural atmosphere as the background
  bool ibl = true;  // sky-driven image based lighting
  f32 ibl_intensity = 1.0f;

  bool rtao = true;  // ray traced ambient occlusion (needs ray query)
  f32 ao_radius = 1.2f;
  f32 ao_intensity = 1.0f;
  u32 ao_rays = 2;

  // Screen-space ambient occlusion. The renderer uses ray-traced ao when it is
  // available and enabled; this is the fallback that runs otherwise (no ray
  // query, or a tier that forces raster ao), so low-end presets still get ao.
  bool ssao = false;

  bool ddgi = true;  // probe based diffuse gi (needs ray query)
  f32 ddgi_spacing = 1.5f;
  f32 ddgi_intensity = 1.0f;

  bool water_reflections = true;  // raytraced; off falls back to sky only

  bool rt_reflections = true;  // raytraced specular for opaque surfaces (needs ray query)
  f32 reflection_roughness_cutoff = 0.6f;  // above this, fall back to prefiltered ibl

  // Screen-space reflections. The non-rt reflection fallback: runs whenever
  // ray-traced reflections are not, so low-end/mobile tiers still get grazing
  // specular off the floor and walls.
  bool ssr = true;

  // Screen-space global illumination. The non-rt diffuse-gi fallback: runs
  // whenever the ddgi probe volume is not, so low-end/mobile tiers still get a
  // bounce of indirect color.
  bool ssgi = true;

  bool path_trace = false;  // path tracer (needs ray query); NRD-denoised + playable by default
  bool path_trace_reference = false;  // force brute-force accumulation (ground truth, no denoise)
  // Lighting samples per pixel in the denoised gbuffer pass. Higher = lower input
  // variance = NRD has to invent less under motion (less shimmer), at a linear
  // cost. Reference mode ignores this (it accumulates over frames instead).
  u32 path_trace_spp = 2;
  // NRD diffuse history length (frames) for the denoised path. Lower = more
  // responsive (less ghosting + shadow lag) but grainier; raise spp to compensate.
  u32 path_trace_accum = 16;
  // Gameplay reconstruction renderer (own SVGF-style temporal + a-trous denoise),
  // a separate mode from reference / NRD.
  bool path_trace_recon = false;
  f32 path_trace_recon_weight = 0.05f;  // temporal: floor on current-frame weight
  u32 path_trace_recon_atrous = 4;      // a-trous wavelet passes
  // ReSTIR GI in the recon mode: spatiotemporal reservoir resampling of the
  // indirect diffuse samples. Big variance drop for one extra visibility ray;
  // off falls back to inline multi-bounce integration.
  bool path_trace_restir = true;
  // ReSTIR DI over the sun disk + dynamic point lights (needs restir).
  bool path_trace_restir_di = true;
  // DLSS Ray Reconstruction as the recon denoiser (needs the NGX dlssd
  // snippet); silently falls back to the in-tree SVGF chain when unavailable.
  bool path_trace_rr = true;
  u32 path_trace_recon_debug = 0;  // 0 final,1 lighting,2 history,3 variance,4 motion,5 normal,6 albedo,7 specular

  // Atmospheric aerial perspective: distant geometry hazes/blue-shifts like the
  // sky, from a camera->surface raymarch of the atmosphere LUTs. 0 disables.
  f32 aerial_perspective = 1.0f;

  // Raymarched volumetric clouds over the sky (procedural, depth-composited).
  bool clouds = true;
  f32 cloud_coverage = 0.46f;  // 0 clear .. 1 overcast

  // Screen-space precipitation, driven by the weather system (0 disables).
  f32 precipitation = 0.0f;  // 0 none .. 1 heavy
  bool precip_snow = false;  // snow flakes vs rain streaks

  // Procedural night-sky aurora (Skyrim's northern lights). On for Skyrim.
  bool aurora = false;

  // Lightning flash intensity this frame (0 none .. 1 peak), driven by the engine
  // during thunderstorms. Boosts the per-frame direct light + clouds.
  f32 lightning = 0.0f;

  // Per-object + camera motion blur (tile-max gather on the prepass velocity).
  bool motion_blur = true;
  f32 motion_blur_shutter = 0.5f;  // 180-degree shutter

  // Lens package: flare ghosts + halo off the bloom chain, radial chromatic
  // aberration, vignette and film grain. Subtle defaults; 0 disables each.
  f32 lens_flare = 0.06f;
  f32 chromatic_aberration = 1.2f;  // px at the corners
  f32 vignette = 0.22f;
  f32 film_grain = 0.015f;

  // Bokeh depth of field on the resolved frame. focus <= 0 = center autofocus.
  // Off by default: the center-chasing focal plane blurs most of a landscape
  // vista and reads as a permanently soft image.
  bool dof = false;
  f32 dof_aperture = 2.8f;    // coc px per unit relative defocus (bigger = shallower)
  f32 dof_focus = 0.0f;       // meters; <= 0 autofocus

  // Screen-space subsurface scattering: separable per-channel diffusion of the
  // skin-flagged materials' diffuse lighting (red bleed at shadow terminators).
  bool sss = true;
  f32 sss_width = 0.012f;  // world scattering radius, meters

  // Unified froxel volumetric lighting: a 3D scattering volume lit by the sun
  // and every clustered light (with local shadows), sampled by the fog
  // composite and translucents. Density is the subtle always-on base haze.
  bool froxel_fog = true;
  f32 froxel_density = 0.005f;

  // Local light shadows: a depth-atlas of faces for the nearest clustered
  // spot/point lights (the raster tier's answer to light leaking; the rt
  // fragment path keeps its exact per-light rays).
  bool local_shadows = true;

  // FFT ocean (Tessendorf): frequency-space wave field inverse-FFT'd into
  // tiling displacement/normal/foam maps, replacing the four Gerstner waves.
  bool fft_ocean = true;

  // Hybrid ReSTIR DI: reservoir-resampled point/spot lighting with one
  // ray-traced shadow ray per pixel, replacing the analytic cluster loop and
  // the local shadow atlas for those lights on opaque surfaces. Needs ray
  // query; experimental (off by default), REC_RESTIR_DI=1.
  bool restir_di = false;

  // Variable rate shading: content-adaptive per-block shading rates on the
  // scene pass, rebuilt each frame from luminance detail + screen motion.
  // Needs attachment VRS hardware; silently full-rate otherwise.
  bool vrs = true;
  f32 vrs_threshold = 0.06f;  // relative luma error allowed for half rate

  // Async compute: overlap self-contained compute (DDGI) with the raster
  // pipeline on a second queue. Needs device support; no-op otherwise.
  bool async_compute = true;

  // FSR3 frame generation: optical-flow + interpolation inserts a generated
  // frame between real ones (two presents per engine frame, FIFO-paced).
  // Experimental; vulkan + SDR swapchain only. REC_FRAMEGEN=1 to enable.
  bool frame_generation = false;

  bool fog = false;  // ray-marched volumetric fog with shadowed sun shafts (needs ray query)
  f32 fog_density = 0.03f;
  f32 fog_height_falloff = 0.15f;
  f32 fog_base_height = 0.0f;
  f32 fog_anisotropy = 0.6f;  // henyey-greenstein g (forward scattering)

  Vec3 sun_direction{-0.35f, -0.9f, -0.25f};  // travel direction of the light
  f32 sun_intensity = 4.0f;
  Vec3 sun_color{1.0f, 0.96f, 0.9f};
  f32 ambient = 0.06f;

  // Authored interior-cell lighting resolved from CELL XCLL + its LGTM template.
  // When `interior` is set the renderer suppresses the sky/atmosphere/clouds/
  // aerial/ocean, replaces sky IBL with the flat ambient, drives the directional
  // fill through the sun path, and fades geometry to the fog colours over the
  // near..far distance. Colours are 0..1 (byte/255); distances are meters.
  bool interior = false;
  Vec3 interior_ambient{0.05f, 0.05f, 0.06f};
  Vec3 interior_directional_color{0.0f, 0.0f, 0.0f};
  Vec3 interior_directional_dir{0.2f, -0.9f, 0.3f};  // engine-space travel dir
  f32 interior_directional_intensity = 0.0f;
  Vec3 interior_fog_near_color{0.0f, 0.0f, 0.0f};
  Vec3 interior_fog_far_color{0.0f, 0.0f, 0.0f};
  f32 interior_fog_near = 0.0f;
  f32 interior_fog_far = 0.0f;   // <= near disables the fog
  f32 interior_fog_power = 1.0f;
  f32 interior_fog_max = 1.0f;

  bool bloom = true;
  f32 bloom_intensity = 0.04f;

  // HDR presentation: request an HDR10/scRGB swapchain (falls back to SDR
  // when the surface has neither); paper white = nits of tonemapped 1.0.
  bool hdr_output = false;
  f32 hdr_paper_white = 200.0f;
  bool auto_exposure = true;
  f32 adaptation_speed = 3.0f;
  // Compensation multiplier under auto exposure, absolute exposure without.
  f32 exposure = 1.0f;
  // AgX default: hue-preserving highlight rolloff (sun-lit surfaces desaturate
  // into white gradually instead of clipping per channel like the ACES fit).
  TonemapOperator tonemap = TonemapOperator::kAgx;
  ColorGrade color_grade = ColorGrade::kNeutral;  // post-tonemap lut grade
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_SETTINGS_H_
