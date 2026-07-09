// Round-trips RenderSettings <-> INI and validates every shipped platform
// preset in engine/render/presets. Run via ctest (settings_initest).

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include "render/core/settings_ini.h"

using rx::render::AntiAliasingMode;
using rx::render::ApplyIni;
using rx::render::LoadSettingsIni;
using rx::render::RenderSettings;
using rx::render::SettingsToIni;
using rx::render::TonemapOperator;
using rx::render::UpscalerKind;
using rx::render::UpscalerQuality;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("  FAIL: %s\n", what);
    ++g_failures;
  }
}

bool Approx(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// Compares every field that the (de)serializer round-trips.
bool CoveredEqual(const RenderSettings& a, const RenderSettings& b) {
  return a.aa_mode == b.aa_mode && a.upscaler == b.upscaler &&
         a.upscaler_quality == b.upscaler_quality && Approx(a.sharpness, b.sharpness) &&
         Approx(a.taa_history_blend, b.taa_history_blend) && a.rt_shadows == b.rt_shadows &&
         Approx(a.sun_angular_radius, b.sun_angular_radius) && a.shadow_maps == b.shadow_maps &&
         a.shadow_resolution == b.shadow_resolution &&
         Approx(a.shadow_distance, b.shadow_distance) && a.gpu_culling == b.gpu_culling &&
         a.gpu_occlusion == b.gpu_occlusion && a.distance_lod == b.distance_lod &&
         a.mesh_shader_lod == b.mesh_shader_lod && a.vsync == b.vsync && a.sky == b.sky &&
         a.ibl == b.ibl && Approx(a.ibl_intensity, b.ibl_intensity) &&
         Approx(a.aerial_perspective, b.aerial_perspective) && a.clouds == b.clouds &&
         Approx(a.cloud_coverage, b.cloud_coverage) && a.rtao == b.rtao && a.ssao == b.ssao &&
         Approx(a.ao_radius, b.ao_radius) && Approx(a.ao_intensity, b.ao_intensity) &&
         a.ao_rays == b.ao_rays && a.ddgi == b.ddgi && Approx(a.ddgi_spacing, b.ddgi_spacing) &&
         Approx(a.ddgi_intensity, b.ddgi_intensity) && a.ssgi == b.ssgi &&
         a.rt_reflections == b.rt_reflections &&
         Approx(a.reflection_roughness_cutoff, b.reflection_roughness_cutoff) &&
         a.water_reflections == b.water_reflections && a.ssr == b.ssr &&
         a.path_trace == b.path_trace && a.path_trace_reference == b.path_trace_reference &&
         a.path_trace_spp == b.path_trace_spp && a.path_trace_accum == b.path_trace_accum &&
         a.path_trace_recon == b.path_trace_recon &&
         Approx(a.path_trace_recon_weight, b.path_trace_recon_weight) &&
         a.path_trace_recon_atrous == b.path_trace_recon_atrous && a.fog == b.fog &&
         Approx(a.fog_density, b.fog_density) &&
         Approx(a.fog_height_falloff, b.fog_height_falloff) &&
         Approx(a.fog_base_height, b.fog_base_height) &&
         Approx(a.fog_anisotropy, b.fog_anisotropy) && a.bloom == b.bloom &&
         Approx(a.bloom_intensity, b.bloom_intensity) && a.auto_exposure == b.auto_exposure &&
         Approx(a.adaptation_speed, b.adaptation_speed) && Approx(a.exposure, b.exposure) &&
         a.tonemap == b.tonemap;
}

void TestRoundTrip() {
  std::printf("round-trip\n");
  RenderSettings a;  // mutate a spread of fields across every type/enum
  a.aa_mode = AntiAliasingMode::kUpscaler;
  a.upscaler = UpscalerKind::kDlss;
  a.upscaler_quality = UpscalerQuality::kBalanced;
  a.sharpness = 0.42f;
  a.rt_shadows = false;
  a.sun_angular_radius = 0.0073f;
  a.shadow_maps = true;
  a.shadow_resolution = 4096;
  a.shadow_distance = 222.0f;
  a.vsync = true;
  a.distance_lod = true;
  a.clouds = false;
  a.rtao = false;
  a.ssao = true;
  a.ao_rays = 7;
  a.ddgi = false;
  a.ddgi_spacing = 2.25f;
  a.rt_reflections = false;
  a.reflection_roughness_cutoff = 0.33f;
  a.ssr = false;
  a.path_trace = true;
  a.path_trace_spp = 5;
  a.path_trace_accum = 31;
  a.fog = true;
  a.fog_density = 0.07f;
  a.bloom = false;
  a.auto_exposure = false;
  a.exposure = 1.7f;
  a.tonemap = TonemapOperator::kReinhard;

  RenderSettings b;  // defaults
  const int applied = ApplyIni(SettingsToIni(a), b);
  Check(applied > 0, "round-trip applied keys");
  Check(CoveredEqual(a, b), "round-trip preserves every covered field");
}

void TestPartialOverlay() {
  std::printf("partial overlay\n");
  RenderSettings s;  // defaults: ddgi_spacing 1.5, ao_rays 2, bloom true
  const int applied = ApplyIni("ao_rays = 5\nbloom = false\n; comment\n[ignored]\n", s);
  Check(applied == 2, "only the two listed keys applied");
  Check(s.ao_rays == 5, "ao_rays overlaid");
  Check(!s.bloom, "bloom overlaid");
  Check(Approx(s.ddgi_spacing, 1.5f), "untouched field keeps its value");
}

void TestEnumAliases() {
  std::printf("enum aliases / case\n");
  RenderSettings s;
  ApplyIni("upscaler = FSR\nupscaler_quality = DLAA\naa_mode = Upscaler\ntonemap = NONE\n", s);
  Check(s.upscaler == UpscalerKind::kFsr3, "FSR alias -> fsr3");
  Check(s.upscaler_quality == UpscalerQuality::kNativeAa, "DLAA alias -> native");
  Check(s.aa_mode == AntiAliasingMode::kUpscaler, "case-insensitive enum");
  Check(s.tonemap == TonemapOperator::kNone, "tonemap none");
}

// Loads a shipped preset and runs a per-file spot check on distinctive values.
void CheckPreset(const std::filesystem::path& dir, const char* name,
                 bool (*pred)(const RenderSettings&)) {
  RenderSettings s;
  const bool ok = LoadSettingsIni(dir / name, s);
  Check(ok, name);
  if (ok) Check(pred(s), name);
}

void TestShippedPresets() {
  std::printf("shipped presets\n");
  const std::filesystem::path dir{RECREATION_PRESETS_TEST_DIR};

  RenderSettings missing;
  Check(!LoadSettingsIni(dir / "does_not_exist.ini", missing), "missing file -> false");

  CheckPreset(dir, "android_low.ini", [](const RenderSettings& s) {
    return !s.rt_shadows && !s.bloom && !s.ssr && !s.ssgi && s.shadow_resolution == 1024 &&
           s.upscaler == UpscalerKind::kNone;
  });
  CheckPreset(dir, "android_high.ini", [](const RenderSettings& s) {
    return !s.rt_shadows && s.ssao && s.ssgi && s.bloom && s.clouds;
  });
  CheckPreset(dir, "steamdeck.ini", [](const RenderSettings& s) {
    return s.upscaler == UpscalerKind::kFsr3 &&
           s.upscaler_quality == UpscalerQuality::kPerformance && s.rt_shadows && s.rtao &&
           !s.rt_reflections && s.vsync;
  });
  CheckPreset(dir, "pc_low.ini", [](const RenderSettings& s) {
    return !s.rt_shadows && s.upscaler == UpscalerKind::kFsr3 && s.ssao && !s.rt_reflections;
  });
  CheckPreset(dir, "pc_medium.ini", [](const RenderSettings& s) {
    return s.rt_shadows && s.rt_reflections && s.ddgi && s.upscaler == UpscalerKind::kDlss;
  });
  CheckPreset(dir, "pc_high.ini", [](const RenderSettings& s) {
    return s.rt_shadows && s.ao_rays == 4 && Approx(s.ddgi_spacing, 1.0f) &&
           s.shadow_resolution == 4096;
  });
  CheckPreset(dir, "pc_ultra.ini", [](const RenderSettings& s) {
    return s.upscaler_quality == UpscalerQuality::kNativeAa && s.ao_rays == 6 &&
           Approx(s.reflection_roughness_cutoff, 0.85f) && s.path_trace_spp == 4;
  });
  CheckPreset(dir, "console.ini", [](const RenderSettings& s) {
    return s.upscaler == UpscalerKind::kFsr3 &&
           s.upscaler_quality == UpscalerQuality::kBalanced && s.vsync &&
           Approx(s.fog_density, 0.02f);
  });
}

}  // namespace

int main() {
  TestRoundTrip();
  TestPartialOverlay();
  TestEnumAliases();
  TestShippedPresets();
  if (g_failures == 0) {
    std::printf("settings_initest: all checks passed\n");
    return 0;
  }
  std::printf("settings_initest: %d check(s) failed\n", g_failures);
  return 1;
}
