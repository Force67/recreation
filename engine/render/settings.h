#ifndef RECREATION_RENDER_SETTINGS_H_
#define RECREATION_RENDER_SETTINGS_H_

#include "core/math.h"
#include "core/types.h"
#include "render/antialiasing.h"
#include "render/upscaler.h"

namespace rec::render {

enum class TonemapOperator : u8 { kAces, kReinhard, kNone };

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
  bool wireframe = false;
  bool vsync = false;

  Vec3 sun_direction{-0.35f, -0.9f, -0.25f};  // travel direction of the light
  f32 sun_intensity = 4.0f;
  Vec3 sun_color{1.0f, 0.96f, 0.9f};
  f32 ambient = 0.06f;

  f32 exposure = 1.0f;
  TonemapOperator tonemap = TonemapOperator::kAces;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_SETTINGS_H_
