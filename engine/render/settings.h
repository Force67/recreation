#ifndef RECREATION_RENDER_SETTINGS_H_
#define RECREATION_RENDER_SETTINGS_H_

#include "core/math.h"
#include "core/types.h"
#include "render/antialiasing.h"
#include "render/upscaler.h"

namespace rec::render {

enum class TonemapOperator : u8 { kAces, kReinhard, kNone };

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

  bool rt_shadows = true;  // masked by device caps and the renderer desc
  f32 sun_angular_radius = 0.005f;  // radians; 0 reverts to hard shadows
  bool wireframe = false;
  bool vsync = false;
  DebugView debug_view = DebugView::kOff;  // isolate a shading channel

  bool sky = true;  // procedural atmosphere as the background
  bool ibl = true;  // sky-driven image based lighting
  f32 ibl_intensity = 1.0f;

  bool rtao = true;  // ray traced ambient occlusion (needs ray query)
  f32 ao_radius = 1.2f;
  f32 ao_intensity = 1.0f;
  u32 ao_rays = 2;

  bool ddgi = true;  // probe based diffuse gi (needs ray query)
  f32 ddgi_spacing = 1.5f;
  f32 ddgi_intensity = 1.0f;

  bool water_reflections = true;  // raytraced; off falls back to sky only

  Vec3 sun_direction{-0.35f, -0.9f, -0.25f};  // travel direction of the light
  f32 sun_intensity = 4.0f;
  Vec3 sun_color{1.0f, 0.96f, 0.9f};
  f32 ambient = 0.06f;

  bool bloom = true;
  f32 bloom_intensity = 0.04f;

  bool auto_exposure = true;
  f32 adaptation_speed = 3.0f;
  // Compensation multiplier under auto exposure, absolute exposure without.
  f32 exposure = 1.0f;
  TonemapOperator tonemap = TonemapOperator::kAces;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_SETTINGS_H_
