#include "render/presets.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>

namespace rec::render {
namespace {

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool IsMobileGpu(const std::string& name) {
  const std::string n = Lower(name);
  for (const char* marker :
       {"adreno", "mali", "powervr", "apple", "xclipse", "immortalis", "vivante"}) {
    if (n.find(marker) != std::string::npos) return true;
  }
  return false;
}

f32 Degrees(f32 deg) { return deg / 57.29578f; }

// Nvidia gets dlss when present, everyone else (amd, intel, handhelds) fsr3.
// The renderer falls back to taa if the chosen backend is not compiled or the
// gpu cannot run it, so this only expresses a preference.
UpscalerKind PreferredUpscaler(const DeviceCaps& caps) {
  return Lower(caps.adapter_name).find("nvidia") != std::string::npos ? UpscalerKind::kDlss
                                                                      : UpscalerKind::kFsr3;
}

}  // namespace

RenderSettings PresetSettings(QualityPreset preset, const DeviceCaps& caps) {
  RenderSettings s;  // sane defaults; each tier overrides from here
  const UpscalerKind up = PreferredUpscaler(caps);

  switch (ResolvePreset(preset, caps)) {
    case QualityPreset::kAndroid:
      // Bandwidth-starved tile gpus: raster only, cheap screen-space ao.
      s.aa_mode = AntiAliasingMode::kTaa;
      s.upscaler = UpscalerKind::kNone;
      s.rt_shadows = s.rtao = s.ddgi = s.rt_reflections = false;
      s.water_reflections = s.fog = false;
      s.bloom = false;
      s.ssao = true;
      s.ao_rays = 1;  // 8 screen-space taps
      s.shadow_resolution = 1024;  // one small cascade atlas fits the power budget
      s.sun_angular_radius = 0.0f;
      break;

    case QualityPreset::kSteamDeck:
      // RDNA2 handheld: ray query exists but the power budget is tiny.
      s.aa_mode = AntiAliasingMode::kUpscaler;
      s.upscaler = UpscalerKind::kFsr3;
      s.upscaler_quality = UpscalerQuality::kPerformance;
      s.rt_shadows = true;
      s.sun_angular_radius = Degrees(0.25f);
      s.rtao = true;
      s.ao_rays = 1;
      s.ssao = true;  // fallback when ray-traced ao is unavailable
      s.ddgi = true;
      s.ddgi_spacing = 2.5f;
      s.rt_reflections = false;
      s.water_reflections = false;
      s.fog = false;
      break;

    case QualityPreset::kLowEnd:
      // Weak/old discrete or rt-less desktops: upscale hard, screen-space ao.
      s.aa_mode = AntiAliasingMode::kUpscaler;
      s.upscaler = UpscalerKind::kFsr3;
      s.upscaler_quality = UpscalerQuality::kPerformance;
      s.rt_shadows = s.rtao = s.ddgi = s.rt_reflections = false;
      s.water_reflections = s.fog = false;
      s.ssao = true;
      s.ao_rays = 2;  // 16 screen-space taps
      break;

    case QualityPreset::kConsole:
      // PS5 / Series X: full hybrid rt tuned for a locked 60.
      s.aa_mode = AntiAliasingMode::kUpscaler;
      s.upscaler = UpscalerKind::kFsr3;
      s.upscaler_quality = UpscalerQuality::kBalanced;
      s.rt_shadows = true;
      s.sun_angular_radius = Degrees(0.5f);
      s.rtao = true;
      s.ao_rays = 3;
      s.ddgi = true;
      s.ddgi_spacing = 1.5f;
      s.rt_reflections = true;
      s.reflection_roughness_cutoff = 0.5f;
      s.water_reflections = true;
      s.fog = true;
      s.fog_density = 0.02f;
      break;

    case QualityPreset::kMedium:
      s.aa_mode = AntiAliasingMode::kUpscaler;
      s.upscaler = up;
      s.upscaler_quality = UpscalerQuality::kQuality;
      s.rt_shadows = true;
      s.sun_angular_radius = Degrees(0.4f);
      s.rtao = true;
      s.ao_rays = 2;
      s.ddgi = true;
      s.ddgi_spacing = 1.5f;
      s.rt_reflections = true;
      s.reflection_roughness_cutoff = 0.4f;
      s.water_reflections = true;
      s.fog = false;
      break;

    case QualityPreset::kHigh:
      s.aa_mode = AntiAliasingMode::kUpscaler;
      s.upscaler = up;
      s.upscaler_quality = UpscalerQuality::kQuality;
      s.rt_shadows = true;
      s.sun_angular_radius = Degrees(0.5f);
      s.rtao = true;
      s.ao_rays = 4;
      s.ddgi = true;
      s.ddgi_spacing = 1.0f;
      s.rt_reflections = true;
      s.reflection_roughness_cutoff = 0.6f;
      s.water_reflections = true;
      s.fog = true;
      break;

    case QualityPreset::kUltra:
      // Flagship: native-res reconstruction (dlaa/native) and every effect.
      s.aa_mode = AntiAliasingMode::kUpscaler;
      s.upscaler = up;
      s.upscaler_quality = UpscalerQuality::kNativeAa;
      s.rt_shadows = true;
      s.sun_angular_radius = Degrees(0.5f);
      s.rtao = true;
      s.ao_rays = 6;
      s.ddgi = true;
      s.ddgi_spacing = 1.0f;
      s.rt_reflections = true;
      s.reflection_roughness_cutoff = 0.85f;
      s.water_reflections = true;
      s.fog = true;
      break;

    case QualityPreset::kAuto:  // already resolved above
      break;
  }

  // Every ray-traced-ao tier keeps the screen-space fallback armed; it only
  // runs when ray-traced ao is actually unavailable (no ray query or no nrd).
  if (s.rtao) s.ssao = true;

  // Clamp to what the device can actually run so a forced preset never hangs.
  if (!caps.ray_query) {
    s.rt_shadows = false;
    s.rtao = false;
    s.ddgi = false;
    s.rt_reflections = false;
    s.water_reflections = false;
    s.fog = false;
    s.path_trace = false;
  }

  // Cascaded shadow maps are the sun-shadow path whenever ray tracing isn't, so
  // every non-rt tier (and forced-low on capable gpus) still casts sun shadows.
  if (!s.rt_shadows) s.shadow_maps = true;
  if (s.upscaler == UpscalerKind::kDlss &&
      Lower(caps.adapter_name).find("nvidia") == std::string::npos) {
    s.upscaler = UpscalerKind::kFsr3;
  }
  if (s.aa_mode == AntiAliasingMode::kUpscaler && s.upscaler == UpscalerKind::kNone) {
    s.aa_mode = AntiAliasingMode::kTaa;
  }
  return s;
}

QualityPreset DetectPreset(const DeviceCaps& caps) {
  if (!caps.ray_query) {
    return IsMobileGpu(caps.adapter_name) ? QualityPreset::kAndroid : QualityPreset::kLowEnd;
  }
  if (caps.integrated) return QualityPreset::kSteamDeck;

  const u64 gib = caps.device_local_bytes >> 30;
  if (gib >= 11) return QualityPreset::kUltra;  // 12 GB+ flagship
  if (gib >= 7) return QualityPreset::kHigh;     // ~8 GB
  return QualityPreset::kMedium;
}

const char* PresetName(QualityPreset preset) {
  switch (preset) {
    case QualityPreset::kAuto: return "auto";
    case QualityPreset::kAndroid: return "android";
    case QualityPreset::kSteamDeck: return "steamdeck";
    case QualityPreset::kLowEnd: return "low";
    case QualityPreset::kConsole: return "console";
    case QualityPreset::kMedium: return "medium";
    case QualityPreset::kHigh: return "high";
    case QualityPreset::kUltra: return "ultra";
  }
  return "auto";
}

QualityPreset ParsePreset(const std::string& name) {
  const std::string n = Lower(name);
  if (n == "android" || n == "mobile") return QualityPreset::kAndroid;
  if (n == "steamdeck" || n == "deck") return QualityPreset::kSteamDeck;
  if (n == "low" || n == "lowend") return QualityPreset::kLowEnd;
  if (n == "console") return QualityPreset::kConsole;
  if (n == "medium" || n == "mid") return QualityPreset::kMedium;
  if (n == "high") return QualityPreset::kHigh;
  if (n == "ultra") return QualityPreset::kUltra;
  return QualityPreset::kAuto;
}

}  // namespace rec::render
