#ifndef RECREATION_RENDER_PRESETS_H_
#define RECREATION_RENDER_PRESETS_H_

#include <string>

#include "core/types.h"
#include "render/rhi/device.h"
#include "render/core/settings.h"

namespace rec::render {

// Hardware quality tiers. kAuto resolves to one of the concrete tiers from the
// device caps at startup; the rest can be forced with --preset or the debug ui.
// Ordered low to high so comparisons (tier >= kConsole) read naturally.
enum class QualityPreset : u8 {
  kAuto,
  kAndroid,    // mobile/tile gpus, usually no ray tracing
  kSteamDeck,  // rdna2 handheld, ray query at a tight power budget
  kLowEnd,     // old or weak discrete gpus, no ray tracing
  kConsole,    // ps5/series-x class, full rt tuned for 60 fps
  kMedium,     // mid-range rt desktop (rtx 3060 / rx 6600)
  kHigh,       // high-end rt desktop (rtx 4070/4080)
  kUltra,      // flagship (rtx 4090/5090), everything on
};

// Tunes a fresh RenderSettings for the tier, then clamps every ray-traced
// feature off when the device cannot do it, so the result is always runnable.
RenderSettings PresetSettings(QualityPreset preset, const DeviceCaps& caps);

// Picks a concrete tier from the gpu class, vram and ray tracing support.
QualityPreset DetectPreset(const DeviceCaps& caps);

// Resolves kAuto, leaves a concrete tier untouched.
inline QualityPreset ResolvePreset(QualityPreset preset, const DeviceCaps& caps) {
  return preset == QualityPreset::kAuto ? DetectPreset(caps) : preset;
}

const char* PresetName(QualityPreset preset);
QualityPreset ParsePreset(const std::string& name);  // kAuto on no match

}  // namespace rec::render

#endif  // RECREATION_RENDER_PRESETS_H_
