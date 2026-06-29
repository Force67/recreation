#include "debug_ui.h"

#include "fly_camera.h"

#if defined(RECREATION_HAS_IMGUI)

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <base/option.h>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include "core/log.h"
#include "render/core/presets.h"
#include "render/core/settings_ini.h"

namespace rec {
namespace {

// Config toggle formerly read from getenv (populated by base::InitOptionsFromEnv).
base::Option<bool> HideDebugUi{"hide.debug.ui", false, "REC_HIDE_DEBUG_UI"};

// Override for the editable .ini render presets directory; defaults to the
// compiled-in engine/render/presets source path.
base::Option<const char*> PresetsDirOpt{"presets.dir", nullptr, "REC_PRESETS_DIR"};

// Directory holding the .ini render presets: REC_PRESETS_DIR, else the
// compiled-in source path, else a cwd-relative fallback.
std::filesystem::path PresetDir() {
  if (const char* env = PresetsDirOpt.get(); env && *env) return env;
#ifdef RECREATION_PRESETS_DIR_DEFAULT
  return std::filesystem::path(RECREATION_PRESETS_DIR_DEFAULT);
#else
  return std::filesystem::path("engine/render/presets");
#endif
}

// A bold sans face for the trailer title cards, rasterized once at a large size
// and drawn downscaled so the giant captions stay crisp. Bold first, then the
// regular weight; null if the system has none (titles fall back to the default).
const char* FindTitleFont() {
  static std::string resolved;
  if (!resolved.empty()) return resolved.c_str();
  static const char* candidates[] = {
#if defined(_WIN32)
      "C:/Windows/Fonts/segoeuib.ttf", "C:/Windows/Fonts/arialbd.ttf",
      "C:/Windows/Fonts/segoeui.ttf",  "C:/Windows/Fonts/arial.ttf",
#else
      "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
      "/run/current-system/sw/share/X11/fonts/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/noto/NotoSans-Bold.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/run/current-system/sw/share/X11/fonts/DejaVuSans.ttf",
#endif
  };
  for (const char* c : candidates)
    if (std::filesystem::exists(c)) {
      resolved = c;
      return resolved.c_str();
    }
  return nullptr;
}

const char* kAaModes[] = {"None", "TAA", "FSR3 Upscaler", "DLSS Upscaler"};
const char* kQualities[] = {"Native AA (1.0x)", "Quality (1.5x)", "Balanced (1.7x)",
                            "Performance (2.0x)"};
const char* kTonemaps[] = {"ACES", "Reinhard", "None"};
const char* kColorGrades[] = {"Neutral", "Warm", "Cool", "Cinematic", "Custom (.cube)"};
const char* kDebugViews[] = {"Off",         "Base color",   "World normal",
                             "Roughness",   "Metallic",     "Ambient occlusion",
                             "Indirect GI", "Direct light", "Emissive", "Reflection",
                             "Overdraw",    "Bounds (BVH)", "Temporal history",
                             "Motion vectors", "Ray count", "Light complexity"};

// FPS readout colour bands: green above smooth, amber in the warning band, red
// once it slips below playable.
constexpr f32 kFpsGood = 60.0f;
constexpr f32 kFpsWarn = 30.0f;

// Row 0 is "Custom" (hand-tuned); the rest map to QualityPreset below.
const char* kPresets[] = {"Custom",  "Auto-detect", "Android", "Steam Deck", "Low end",
                          "Console", "Medium",      "High",    "Ultra"};
const render::QualityPreset kPresetValues[] = {
    render::QualityPreset::kAuto,      // unused for row 0
    render::QualityPreset::kAuto,      render::QualityPreset::kAndroid,
    render::QualityPreset::kSteamDeck, render::QualityPreset::kLowEnd,
    render::QualityPreset::kConsole,   render::QualityPreset::kMedium,
    render::QualityPreset::kHigh,      render::QualityPreset::kUltra};

}  // namespace

DebugUi::DebugUi() = default;
DebugUi::~DebugUi() { Shutdown(); }

bool DebugUi::Initialize(Window& window, render::Renderer& renderer) {
  SDL_Window* sdl_window = static_cast<SDL_Window*>(window.native_handles().window);
  render::Device* device = renderer.device();
  if (!sdl_window || !device || device->is_stub()) return false;

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
  io.IniFilename = nullptr;  // no imgui.ini litter next to the binary
  ImGui::StyleColorsDark();

  // Font 0 stays the small default UI face; font 1 (if a system TTF is found) is
  // the large face the trailer titles render with, downscaled for crispness.
  io.Fonts->AddFontDefault();
  if (const char* title_path = FindTitleFont())
    title_font_ = io.Fonts->AddFontFromFileTTF(title_path, 116.0f);

  if (!ImGui_ImplSDL3_InitForVulkan(sdl_window)) return false;

  swapchain_format_ = renderer.swapchain_format();
  ImGui_ImplVulkan_InitInfo info{};
  info.ApiVersion = VK_API_VERSION_1_3;
  info.Instance = device->instance();
  info.PhysicalDevice = device->physical_device();
  info.Device = device->device();
  info.QueueFamily = device->graphics_family();
  info.Queue = device->graphics_queue();
  info.DescriptorPoolSize = 64;  // backend manages its own pool
  info.MinImageCount = std::max(2u, renderer.swapchain_image_count());
  info.ImageCount = std::max(2u, renderer.swapchain_image_count());
  info.UseDynamicRendering = true;
  info.PipelineInfoMain.PipelineRenderingCreateInfo = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
  info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
  info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swapchain_format_;
  if (!ImGui_ImplVulkan_Init(&info)) {
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    return false;
  }

  window.set_event_hook([](const void* event) {
    ImGui_ImplSDL3_ProcessEvent(static_cast<const SDL_Event*>(event));
  });

  // REC_HIDE_DEBUG_UI starts with the imgui overlays hidden, so the libultragui
  // HUD has the screen to itself for clean screenshots (cf. RECREATION_UI_MENU).
  if (HideDebugUi)
    visible_ = trace_visible_ = quests_visible_ = false;
  initialized_ = true;
  REC_INFO("imgui {} initialized (vulkan dynamic rendering)", IMGUI_VERSION);
  return true;
}

void DebugUi::Shutdown() {
  if (!initialized_) return;
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  initialized_ = false;
}

void DebugUi::BeginFrame() {
  if (!initialized_) return;
  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  ImGui::NewFrame();
}

bool DebugUi::wants_mouse() const {
  return initialized_ && ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse;
}

bool DebugUi::wants_keyboard() const {
  return initialized_ && ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureKeyboard;
}

void DebugUi::Build(render::Renderer& renderer, FlyCamera& camera, f32 frame_delta,
                    render::FrameView* view, QuestPanel* quests, NativeTracePanel* trace) {
  if (!initialized_) return;

  frame_times_[frame_time_cursor_] = frame_delta * 1000.0f;
  frame_time_cursor_ = (frame_time_cursor_ + 1) % IM_ARRAYSIZE(frame_times_);

  if (visible_) {
    render::RenderSettings& settings = renderer.settings();
    const render::DeviceCaps* caps = renderer.caps();

    ImGui::SetNextWindowPos({16, 16}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({380, 640}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Renderer (F1 hides)")) {
      if (caps) ImGui::TextWrapped("%s", caps->adapter_name.c_str());
      ImGui::Text("output %ux%u  render %ux%u", renderer.output_width(),
                  renderer.output_height(), renderer.render_width(), renderer.render_height());
      // The fps figure turns amber then red as it drops past the thresholds.
      const f32 fps = frame_delta > 0 ? 1.0f / frame_delta : 0.0f;
      const ImVec4 fps_col = fps >= kFpsGood   ? ImVec4{0.45f, 0.92f, 0.45f, 1.0f}
                             : fps >= kFpsWarn ? ImVec4{0.97f, 0.80f, 0.30f, 1.0f}
                                               : ImVec4{0.97f, 0.33f, 0.28f, 1.0f};
      ImGui::Text("%.2f ms", frame_delta * 1000.0f);
      ImGui::SameLine();
      ImGui::TextColored(fps_col, "(%.0f fps)", fps);
      ImGui::PlotLines("##frametimes", frame_times_, IM_ARRAYSIZE(frame_times_),
                       static_cast<int>(frame_time_cursor_), nullptr, 0.0f, 33.3f,
                       {ImGui::GetContentRegionAvail().x, 48});

      // One-click hardware tier: overwrites every feature toggle below. Touching
      // any of them afterwards just leaves this on the chosen row (still custom).
      if (ImGui::Combo("Quality preset", &preset_choice_, kPresets, IM_ARRAYSIZE(kPresets)) &&
          preset_choice_ > 0 && caps) {
        render::QualityPreset preset = kPresetValues[preset_choice_];
        settings = render::PresetSettings(preset, *caps);
        if (preset == render::QualityPreset::kAuto) {
          REC_INFO("preset: auto -> {}", render::PresetName(render::DetectPreset(*caps)));
        }
      }

      // Editable per-platform .ini presets (engine/render/presets). Loaded
      // straight onto the live settings; "for now" the debug ui is the only way
      // in. Save writes the current settings back out so users can author more.
      if (ImGui::CollapsingHeader("Platform preset (.ini)")) {
        if (!preset_files_scanned_) ScanPresetFiles();
        if (preset_files_.empty()) {
          ImGui::TextDisabled("no .ini in %s", PresetDir().string().c_str());
        } else {
          std::vector<const char*> names;
          names.reserve(preset_files_.size());
          for (const auto& f : preset_files_) names.push_back(f.c_str());
          ImGui::Combo("File", &preset_file_choice_, names.data(), static_cast<int>(names.size()));
          if (ImGui::Button("Load")) {
            const auto path = PresetDir() / preset_files_[preset_file_choice_];
            if (render::LoadSettingsIni(path, settings)) {
              preset_choice_ = 0;  // settings are file-tuned now, not a hardware tier
              preset_status_ = "loaded " + preset_files_[preset_file_choice_];
              REC_INFO("render preset: loaded {}", path.string());
            } else {
              preset_status_ = "could not open " + preset_files_[preset_file_choice_];
            }
          }
          ImGui::SameLine();
        }
        if (ImGui::Button("Rescan")) ScanPresetFiles();
        ImGui::SetNextItemWidth(150);
        ImGui::InputText("##presetname", preset_save_name_, sizeof(preset_save_name_));
        ImGui::SameLine();
        if (ImGui::Button("Save")) {
          std::string fn = preset_save_name_[0] ? preset_save_name_ : "custom";
          if (fn.size() < 4 || fn.compare(fn.size() - 4, 4, ".ini") != 0) fn += ".ini";
          const auto path = PresetDir() / fn;
          if (render::SaveSettingsIni(path, settings)) {
            preset_status_ = "saved " + fn;
            REC_INFO("render preset: saved {}", path.string());
            ScanPresetFiles();
          } else {
            preset_status_ = "could not write " + fn;
          }
        }
        if (!preset_status_.empty()) ImGui::TextDisabled("%s", preset_status_.c_str());
      }

      const auto& timings = renderer.pass_timings();
      if (!timings.empty()) {
        if (ImGui::CollapsingHeader("GPU passes", ImGuiTreeNodeFlags_DefaultOpen)) {
          ImGui::Text("gpu frame %.2f ms", renderer.gpu_frame_ms());
          if (ImGui::BeginTable("passes", 2,
                                ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            for (const auto& t : timings) {
              ImGui::TableNextRow();
              ImGui::TableNextColumn();
              ImGui::TextUnformatted(t.name.c_str());
              ImGui::TableNextColumn();
              ImGui::Text("%.3f ms", t.ms);
            }
            ImGui::EndTable();
          }
        }
      }

      if (render::Device* device = renderer.device();
          device && ImGui::CollapsingHeader("GPU memory")) {
        render::Device::MemoryBudget mem = device->memory_budget();
        const f64 mb = 1.0 / (1024.0 * 1024.0);
        ImGui::Text("used %.0f / %.0f MB", mem.used_bytes * mb, mem.budget_bytes * mb);
        if (mem.budget_bytes > 0) {
          ImGui::ProgressBar(static_cast<f32>(static_cast<f64>(mem.used_bytes) / mem.budget_bytes),
                             {-1, 0});
        }
        ImGui::Text("%u allocations, %.0f MB live", mem.allocation_count,
                    mem.allocated_bytes * mb);
        const render::RenderGraph::Stats& g = renderer.graph_stats();
        ImGui::Text("frame graph transients: %u (%.1f MB)", g.transient_count,
                    g.transient_bytes * mb);
        ImGui::Text("opaque draws: %u / %u visible (gpu cull)", renderer.draws_visible(),
                    renderer.draws_total());
        if (renderer.meshlets_total() > 0) {
          ImGui::Text("meshlets: %u / %u drawn (cluster cull)", renderer.meshlets_visible(),
                      renderer.meshlets_total());
        }
      }

      if (const render::RenderGraph::Stats& g = renderer.graph_stats();
          !g.passes.empty() && ImGui::CollapsingHeader("Frame graph")) {
        ImGui::Text("%zu passes, %u barriers", g.passes.size(), g.barrier_count);
        if (ImGui::BeginTable("fg_passes", 4,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingStretchProp,
                              {0, 180})) {
          ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn("R", ImGuiTableColumnFlags_WidthFixed, 24);
          ImGui::TableSetupColumn("W", ImGuiTableColumnFlags_WidthFixed, 24);
          ImGui::TableSetupColumn("Bar", ImGuiTableColumnFlags_WidthFixed, 30);
          for (const auto& p : g.passes) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p.name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%u", p.reads);
            ImGui::TableNextColumn();
            ImGui::Text("%u", p.writes);
            ImGui::TableNextColumn();
            ImGui::Text("%u", p.barriers);
          }
          ImGui::EndTable();
        }
      }

      if (ImGui::CollapsingHeader("Anti-aliasing & upscaling",
                                  ImGuiTreeNodeFlags_DefaultOpen)) {
        int aa = settings.aa_mode == render::AntiAliasingMode::kNone   ? 0
                 : settings.aa_mode == render::AntiAliasingMode::kTaa   ? 1
                 : settings.upscaler == render::UpscalerKind::kDlss     ? 3
                                                                        : 2;
        if (ImGui::Combo("Mode", &aa, kAaModes, IM_ARRAYSIZE(kAaModes))) {
          switch (aa) {
            case 0:
              settings.aa_mode = render::AntiAliasingMode::kNone;
              settings.upscaler = render::UpscalerKind::kNone;
              break;
            case 1:
              settings.aa_mode = render::AntiAliasingMode::kTaa;
              settings.upscaler = render::UpscalerKind::kNone;
              break;
            case 2:
              settings.upscaler = render::UpscalerKind::kFsr3;
              settings.aa_mode = render::AntiAliasingMode::kUpscaler;
              break;
            case 3:
              settings.upscaler = render::UpscalerKind::kDlss;
              settings.aa_mode = render::AntiAliasingMode::kUpscaler;
              break;
          }
        }
        if (settings.aa_mode == render::AntiAliasingMode::kUpscaler &&
            settings.upscaler != render::UpscalerKind::kNone) {
          int quality = static_cast<int>(settings.upscaler_quality);
          if (ImGui::Combo("Quality", &quality, kQualities, IM_ARRAYSIZE(kQualities))) {
            settings.upscaler_quality = static_cast<render::UpscalerQuality>(quality);
          }
          ImGui::SliderFloat("Sharpness", &settings.sharpness, 0.0f, 1.0f);
          if (!renderer.upscaler_active()) {
            ImGui::TextColored({1, 0.6f, 0.3f, 1}, "upscaler unavailable, taa fallback");
          }
        }
        if (settings.aa_mode == render::AntiAliasingMode::kTaa) {
          ImGui::SliderFloat("History blend", &settings.taa_history_blend, 0.5f, 0.98f);
        }
        // Render scale (internal resolution). Only meaningful without an upscaler
        // driving the resolution; >1 supersamples and the post pass downscales to
        // the window. Committed on release so dragging does not resize every frame.
        if (settings.aa_mode != render::AntiAliasingMode::kUpscaler) {
          ImGui::SliderFloat("Render scale", &render_scale_ui_, 0.5f, 2.0f, "%.2fx");
          if (ImGui::IsItemDeactivatedAfterEdit()) {
            settings.render_scale = render_scale_ui_;
          } else if (!ImGui::IsItemActive()) {
            render_scale_ui_ = settings.render_scale;  // mirror preset/external changes
          }
          if (settings.render_scale > 1.001f) {
            ImGui::SameLine();
            ImGui::TextDisabled("SSAA");
          }
        }
      }

      if (ImGui::CollapsingHeader("Features", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool ray_query = caps && caps->ray_query;
        ImGui::BeginDisabled(!ray_query);
        ImGui::Checkbox("Raytraced shadows", &settings.rt_shadows);
        if (settings.rt_shadows) {
          f32 degrees = settings.sun_angular_radius * 57.29578f;
          if (ImGui::SliderFloat("Sun radius (deg)", &degrees, 0.0f, 2.0f, "%.2f")) {
            settings.sun_angular_radius = degrees / 57.29578f;
          }
        }
        ImGui::EndDisabled();
        ImGui::Checkbox("Cascaded shadow maps", &settings.shadow_maps);
        if (settings.shadow_maps) {
          ImGui::SliderFloat("Shadow distance", &settings.shadow_distance, 30.0f, 400.0f, "%.0f");
          ImGui::TextDisabled(caps && caps->ray_query && settings.rt_shadows
                                  ? "(rt shadows active; cascades are the fallback)"
                                  : "raster sun shadows");
        }
        ImGui::BeginDisabled(!ray_query);
        ImGui::Checkbox("Water RT reflections", &settings.water_reflections);
        ImGui::Checkbox("RT reflections", &settings.rt_reflections);
        if (settings.rt_reflections) {
          ImGui::SliderFloat("Reflection roughness", &settings.reflection_roughness_cutoff, 0.05f,
                             1.0f, "%.2f");
        }
        ImGui::Checkbox("Screen-space reflections", &settings.ssr);
        ImGui::Checkbox("Screen-space GI", &settings.ssgi);
        ImGui::Checkbox("Path tracing", &settings.path_trace);
        if (settings.path_trace) {
          ImGui::Checkbox("  Ground truth (no denoise)", &settings.path_trace_reference);
          if (!settings.path_trace_reference)
            ImGui::Checkbox("  Reconstruction renderer (SVGF)", &settings.path_trace_recon);
          if (settings.path_trace_reference) {
            ImGui::Text("  accumulated %u spp", renderer.path_trace_samples());
          } else if (settings.path_trace_recon) {
            const char* dbg[] = {"Final", "Lighting", "History len", "Variance",
                                 "Motion", "Normal", "Albedo", "Specular"};
            int d = static_cast<int>(settings.path_trace_recon_debug);
            if (ImGui::Combo("  Debug view", &d, dbg, IM_ARRAYSIZE(dbg)))
              settings.path_trace_recon_debug = static_cast<u32>(d);
            int ap = static_cast<int>(settings.path_trace_recon_atrous);
            if (ImGui::SliderInt("  A-trous passes", &ap, 0, 6))
              settings.path_trace_recon_atrous = static_cast<u32>(ap);
            ImGui::SliderFloat("  Responsiveness", &settings.path_trace_recon_weight, 0.01f, 0.5f);
            int spp = static_cast<int>(settings.path_trace_spp);
            if (ImGui::SliderInt("  Samples/pixel ", &spp, 1, 8))
              settings.path_trace_spp = static_cast<u32>(spp);
            ImGui::Checkbox("  ReSTIR DI (point lights)", &settings.path_trace_restir);
          } else {
            // More samples = lower input noise = less motion shimmer, at linear cost.
            int spp = static_cast<int>(settings.path_trace_spp);
            if (ImGui::SliderInt("  Samples/pixel", &spp, 1, 8))
              settings.path_trace_spp = static_cast<u32>(spp);
            // Lower accum = less ghosting/shadow lag but grainier (raise spp to compensate).
            int accum = static_cast<int>(settings.path_trace_accum);
            if (ImGui::SliderInt("  Denoiser history", &accum, 2, 48))
              settings.path_trace_accum = static_cast<u32>(accum);
          }
        }
        ImGui::Checkbox("Volumetric fog", &settings.fog);
        if (settings.fog) {
          ImGui::SliderFloat("Fog density", &settings.fog_density, 0.0f, 0.2f, "%.3f");
          ImGui::SliderFloat("Fog height falloff", &settings.fog_height_falloff, 0.0f, 1.0f);
          ImGui::SliderFloat("Fog anisotropy", &settings.fog_anisotropy, 0.0f, 0.95f);
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!caps || !caps->fill_mode_non_solid);
        ImGui::Checkbox("Wireframe", &settings.wireframe);
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!caps || !caps->mesh_shaders);
        ImGui::Checkbox("Mesh-shader LOD path", &settings.mesh_shader_lod);
        ImGui::EndDisabled();
        if (caps && !caps->mesh_shaders) {
          ImGui::SameLine();
          ImGui::TextDisabled("(no VK_EXT_mesh_shader)");
        }
        ImGui::Checkbox("VSync", &settings.vsync);
        int debug_view = static_cast<int>(settings.debug_view);
        if (ImGui::Combo("Debug view", &debug_view, kDebugViews, IM_ARRAYSIZE(kDebugViews))) {
          settings.debug_view = static_cast<render::DebugView>(debug_view);
        }
      }

      if (ImGui::CollapsingHeader("Global illumination", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool ray_query = caps && caps->ray_query;
        ImGui::Checkbox("Image based lighting", &settings.ibl);
        if (settings.ibl) {
          ImGui::SliderFloat("IBL intensity", &settings.ibl_intensity, 0.0f, 4.0f);
        }
        ImGui::BeginDisabled(!ray_query || !settings.ibl);
        ImGui::Checkbox("DDGI probes", &settings.ddgi);
        if (settings.ddgi) {
          ImGui::SliderFloat("Probe spacing", &settings.ddgi_spacing, 0.5f, 5.0f);
          ImGui::SliderFloat("GI intensity", &settings.ddgi_intensity, 0.0f, 4.0f);
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!ray_query);
        ImGui::Checkbox("RT ambient occlusion", &settings.rtao);
        ImGui::EndDisabled();
        ImGui::Checkbox("Screen-space AO (fallback)", &settings.ssao);
        if (settings.rtao || settings.ssao) {
          ImGui::SliderFloat("AO radius", &settings.ao_radius, 0.2f, 5.0f);
          ImGui::SliderFloat("AO intensity", &settings.ao_intensity, 0.2f, 3.0f);
          int rays = static_cast<int>(settings.ao_rays);
          if (ImGui::SliderInt("AO rays/taps", &rays, 1, 8)) settings.ao_rays = rays;
        }
      }

      if (ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Day/night cycle: scrub the time of day (drives the sun, sky and
        // ambient) and the rate game time passes. While the cycle runs it owns
        // the sun, so the manual sun controls below only stick when time is
        // frozen (Time scale 0) or REC_SUN_DIR pinned a fixed sun.
        if (clock_) {
          f32 hour = clock_->hour();
          if (ImGui::SliderFloat("Time of day", &hour, 0.0f, 24.0f, "%.2f h"))
            clock_->set_hour(hour);
          f32 timescale = clock_->timescale();
          if (ImGui::SliderFloat("Time scale", &timescale, 0.0f, 1200.0f, "%.0fx"))
            clock_->set_timescale(timescale);
        }
        ImGui::Checkbox("Procedural sky", &settings.sky);
        f32 direction[3] = {settings.sun_direction.x, settings.sun_direction.y,
                            settings.sun_direction.z};
        if (ImGui::SliderFloat3("Sun direction", direction, -1.0f, 1.0f)) {
          settings.sun_direction = {direction[0], direction[1], direction[2]};
          if (settings.sun_direction.y > -0.05f) settings.sun_direction.y = -0.05f;
        }
        ImGui::SliderFloat("Sun intensity", &settings.sun_intensity, 0.0f, 20.0f);
        f32 color[3] = {settings.sun_color.x, settings.sun_color.y, settings.sun_color.z};
        if (ImGui::ColorEdit3("Sun color", color)) {
          settings.sun_color = {color[0], color[1], color[2]};
        }
        ImGui::SliderFloat("Ambient", &settings.ambient, 0.0f, 0.5f);
      }

      // Live weather playground: override the climate and drive the sky/clouds/
      // rain directly. The engine applies *weather_state_ while the toggle is on.
      if (weather_enable_ && weather_state_ &&
          ImGui::CollapsingHeader("Weather", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Override climate (live)", weather_enable_);
        weather::WeatherState& w = *weather_state_;
        auto preset = [&](weather::WeatherDef::Kind k) {
          weather::WeatherDef d;
          d.kind = k;
          d.DeriveFromKind();
          w = weather::ToState(d);
          *weather_enable_ = true;
        };
        if (ImGui::Button("Clear")) preset(weather::WeatherDef::Kind::kPleasant);
        ImGui::SameLine();
        if (ImGui::Button("Cloudy")) preset(weather::WeatherDef::Kind::kCloudy);
        ImGui::SameLine();
        if (ImGui::Button("Rainy")) preset(weather::WeatherDef::Kind::kRainy);
        ImGui::SameLine();
        if (ImGui::Button("Snow")) preset(weather::WeatherDef::Kind::kSnow);
        ImGui::SliderFloat("Cloud coverage", &w.cloud_coverage, 0.0f, 1.0f);
        ImGui::SliderFloat("Precipitation", &w.precipitation, 0.0f, 1.0f);
        ImGui::Checkbox("Snow (vs rain)", &w.snow);
        ImGui::SliderFloat("Haze", &w.aerosol, 0.0f, 1.0f);
        ImGui::SliderFloat("Light dimming", &w.light_scale, 0.1f, 1.0f);
      }

      if (ImGui::CollapsingHeader("Post processing", ImGuiTreeNodeFlags_DefaultOpen)) {
        int tonemap = static_cast<int>(settings.tonemap);
        if (ImGui::Combo("Tonemap", &tonemap, kTonemaps, IM_ARRAYSIZE(kTonemaps))) {
          settings.tonemap = static_cast<render::TonemapOperator>(tonemap);
        }
        int grade = static_cast<int>(settings.color_grade);
        if (ImGui::Combo("Color grade", &grade, kColorGrades, IM_ARRAYSIZE(kColorGrades))) {
          settings.color_grade = static_cast<render::ColorGrade>(grade);
        }
        ImGui::Checkbox("Bloom", &settings.bloom);
        if (settings.bloom) {
          ImGui::SliderFloat("Bloom intensity", &settings.bloom_intensity, 0.0f, 0.2f, "%.3f");
        }
        ImGui::Checkbox("Auto exposure", &settings.auto_exposure);
        if (settings.auto_exposure) {
          ImGui::SliderFloat("Adaptation speed", &settings.adaptation_speed, 0.5f, 10.0f);
        }
        ImGui::SliderFloat(settings.auto_exposure ? "Compensation" : "Exposure",
                           &settings.exposure, 0.1f, 8.0f, "%.2f",
                           ImGuiSliderFlags_Logarithmic);
      }

      if (ImGui::CollapsingHeader("Physics")) {
        ImGui::TextDisabled("F throws a floating cube (jolt buoyancy)");
      }

      if (ImGui::CollapsingHeader("Camera")) {
        Vec3 position = camera.position();
        ImGui::Text("position %.1f %.1f %.1f", position.x, position.y, position.z);
        ImGui::SliderFloat("Speed", &camera.speed, 0.1f, 50.0f, "%.1f",
                           ImGuiSliderFlags_Logarithmic);
        ImGui::TextDisabled("RMB look, WASD move, Q/E down/up, shift fast");
      }

      if (ImGui::CollapsingHeader("Scene")) {
        ImGui::Text("meshes %u", renderer.mesh_count());
        if (const render::MaterialSystem* materials = renderer.materials()) {
          ImGui::Text("materials %u, textures %u", materials->material_count(),
                      materials->texture_count());
        }
        ImGui::Checkbox("ImGui demo window", &show_demo_);
      }

      // The quest debugger is its own window (F3), built after this one.
    }
    ImGui::End();

    DrawStageChart(renderer);

    if (show_demo_) ImGui::ShowDemoWindow(&show_demo_);
  }

  // The quest debugger (F3): a dedicated window so it stays reachable without
  // scrolling past the render settings, and toggles independently of them.
  if (quests_visible_ && quests && quests->available) {
    // Right of the renderer and trace windows so the default layout never
    // overlaps them.
    ImGui::SetNextWindowPos({760, 16}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({440, 520}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Quest debugger (F3 hides)", &quests_visible_)) RenderQuestPanel(quests);
    ImGui::End();
  }

  // Separate, independently-toggled window (F2): the recently invoked Papyrus
  // native functions, newest first, plus a busiest-natives tally.
  if (trace_visible_ && trace && trace->available) {
    ImGui::SetNextWindowPos({410, 16}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({340, 460}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Papyrus natives (F2 hides)", &trace_visible_)) {
      ImGui::Text("%llu native calls", static_cast<unsigned long long>(trace->total));
      ImGui::SameLine();
      if (ImGui::SmallButton("Clear") && trace->clear) trace->clear();

      if (ImGui::BeginTabBar("trace_tabs")) {
        if (ImGui::BeginTabItem("Recent")) {
          if (ImGui::BeginChild("recent", {0, 0})) {
            for (const std::string& call : trace->recent) ImGui::TextUnformatted(call.c_str());
          }
          ImGui::EndChild();
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Busiest")) {
          if (ImGui::BeginTable("busiest", 2,
                                ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                    ImGuiTableFlags_SizingStretchProp)) {
            for (const auto& [name, count] : trace->top) {
              ImGui::TableNextRow();
              ImGui::TableNextColumn();
              ImGui::TextUnformatted(name.c_str());
              ImGui::TableNextColumn();
              ImGui::Text("%u", count);
            }
            ImGui::EndTable();
          }
          ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
      }
    }
    ImGui::End();
  }

  DrawTrailerOverlay();  // cinematic chrome on the foreground list, panels or not

  ImGui::Render();
  if (visible_ || ImGui::GetDrawData()->TotalVtxCount > 0) {
    view->ui_draw = [](VkCommandBuffer cmd) {
      ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    };
  }
}

void DebugUi::DrawStageChart(render::Renderer& renderer) {
  const auto& timings = renderer.pass_timings();
  if (timings.empty()) return;

  // Heaviest stages first; fold the long tail into a single "other" bar so the
  // chart stays legible.
  struct Bar {
    const char* name;
    f32 ms;
  };
  std::vector<Bar> bars;
  bars.reserve(timings.size());
  for (const auto& t : timings) bars.push_back({t.name.c_str(), t.ms});
  std::sort(bars.begin(), bars.end(), [](const Bar& a, const Bar& b) { return a.ms > b.ms; });

  constexpr int kMaxBars = 6;
  f32 other_ms = 0.0f;
  if (static_cast<int>(bars.size()) > kMaxBars) {
    for (size_t i = kMaxBars; i < bars.size(); ++i) other_ms += bars[i].ms;
    bars.resize(kMaxBars);
  }
  if (other_ms > 0.0f) bars.push_back({"other", other_ms});
  if (bars.empty()) return;

  const f32 max_ms = bars.front().ms > 0.0f ? bars.front().ms : 1.0f;

  // Geometry, in screen space: this is a HUD pinned to the bottom-left, not a
  // movable window.
  const ImVec2 screen = ImGui::GetIO().DisplaySize;
  const f32 margin = 16.0f;
  const f32 pad = 12.0f;
  const f32 row_h = 16.0f;
  const f32 row_gap = 5.0f;
  const f32 header_h = 24.0f;
  const f32 panel_w = 326.0f;
  const int rows = static_cast<int>(bars.size());
  const f32 panel_h = pad + header_h + rows * row_h + (rows - 1) * row_gap + pad;

  const ImVec2 p0 = {margin, screen.y - margin - panel_h};
  const ImVec2 p1 = {p0.x + panel_w, screen.y - margin};
  ImDrawList* dl = ImGui::GetForegroundDrawList();

  // Dark glass panel with a hairline border.
  dl->AddRectFilled(p0, p1, IM_COL32(14, 16, 22, 214), 9.0f);
  dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 26), 9.0f, 0, 1.0f);

  const f32 content_x = p0.x + pad;
  const f32 content_w = panel_w - 2.0f * pad;
  const f32 text_h = ImGui::GetTextLineHeight();

  // Header: title on the left, total GPU frame time on the right.
  ImFont* font = ImGui::GetFont();
  const f32 font_size = ImGui::GetFontSize();
  const f32 head_y = p0.y + pad;
  dl->AddText(font, font_size, {content_x, head_y}, IM_COL32(206, 214, 232, 255), "GPU STAGES");
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f ms", renderer.gpu_frame_ms());
  const ImVec2 total_sz = ImGui::CalcTextSize(buf);
  dl->AddText(font, font_size, {p1.x - pad - total_sz.x, head_y}, IM_COL32(170, 182, 205, 255),
              buf);

  auto lerp8 = [](int a, int b, f32 t) { return static_cast<int>(a + (b - a) * t + 0.5f); };

  f32 y = p0.y + pad + header_h;
  for (const Bar& bar : bars) {
    const f32 frac = bar.ms / max_ms;
    const f32 fclamp = frac < 0.0f ? 0.0f : (frac > 1.0f ? 1.0f : frac);

    // Track, then a cost-graded fill (teal when cheap, coral when it dominates).
    dl->AddRectFilled({content_x, y}, {content_x + content_w, y + row_h},
                      IM_COL32(255, 255, 255, 16), 3.5f);
    const f32 fill_w = content_w * fclamp;
    if (fill_w > 1.0f) {
      const ImU32 col = IM_COL32(lerp8(78, 240, fclamp), lerp8(201, 104, fclamp),
                                 lerp8(176, 82, fclamp), 235);
      dl->AddRectFilled({content_x, y}, {content_x + fill_w, y + row_h}, col, 3.5f);
    }

    const f32 ty = y + (row_h - text_h) * 0.5f;
    std::snprintf(buf, sizeof(buf), "%.2f", bar.ms);
    const ImVec2 vsz = ImGui::CalcTextSize(buf);
    const f32 val_x = content_x + content_w - 6.0f - vsz.x;
    // Clip the label so a long pass name never runs into the ms value.
    const ImVec4 clip = {content_x + 6.0f, y, val_x - 4.0f, y + row_h};
    dl->AddText(font, font_size, {content_x + 6.0f, ty}, IM_COL32(236, 240, 248, 255), bar.name,
                nullptr, 0.0f, &clip);
    dl->AddText(font, font_size, {val_x, ty}, IM_COL32(232, 237, 245, 255), buf);

    y += row_h + row_gap;
  }
}

void DebugUi::DrawTrailerOverlay() {
  if (!trailer_ || !trailer_->active) return;
  const TrailerOverlay& t = *trailer_;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImVec2 screen = ImGui::GetIO().DisplaySize;
  const f32 W = screen.x, H = screen.y;
  ImFont* font = title_font_ ? title_font_ : ImGui::GetFont();

  // Text with a soft drop shadow; sizes measured in the title font so layout is
  // exact. All sizes are fractions of screen height so it scales to any output.
  auto draw_text = [&](f32 size, ImVec2 pos, ImU32 rgb, f32 alpha, const char* s) {
    if (alpha <= 0.003f || !s || !*s) return;
    const int a = static_cast<int>(alpha * 255.0f + 0.5f);
    const ImU32 fg = (rgb & 0x00FFFFFFu) | (static_cast<ImU32>(a) << 24);
    const ImU32 sh = IM_COL32(0, 0, 0, static_cast<int>(alpha * 170.0f));
    const f32 o = std::max(1.0f, size * 0.045f);
    dl->AddText(font, size, {pos.x + o, pos.y + o}, sh, s);
    dl->AddText(font, size, pos, fg, s);
  };
  auto measure = [&](f32 size, const char* s) {
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, s);
  };

  const ImU32 kWhite = IM_COL32(245, 247, 255, 255);
  const ImU32 kGold = IM_COL32(255, 203, 107, 255);

  // Cinematic letterbox bars (under everything).
  const f32 bar = H * 0.115f * t.letterbox;
  if (bar > 0.5f) {
    dl->AddRectFilled({0, 0}, {W, bar}, IM_COL32(0, 0, 0, 255));
    dl->AddRectFilled({0, H - bar}, {W, H}, IM_COL32(0, 0, 0, 255));
  }
  // Intro / outro black wash over the 3D scene (under the text).
  if (t.fade > 0.003f)
    dl->AddRectFilled({0, 0}, {W, H}, IM_COL32(0, 0, 0, static_cast<int>(t.fade * 255.0f)));

  // Opening title card, centered.
  if (t.intro_alpha > 0.003f) {
    const f32 ts = H * 0.085f;
    const f32 ss = H * 0.026f;
    const ImVec2 tz = measure(ts, t.intro_title.c_str());
    draw_text(ts, {(W - tz.x) * 0.5f, H * 0.40f}, kWhite, t.intro_alpha, t.intro_title.c_str());
    if (!t.intro_subtitle.empty()) {
      const ImVec2 sz = measure(ss, t.intro_subtitle.c_str());
      draw_text(ss, {(W - sz.x) * 0.5f, H * 0.40f + tz.y + H * 0.012f}, kGold, t.intro_alpha,
                t.intro_subtitle.c_str());
    }
  }

  // Lower-third location title, left-aligned just above the bottom bar, with a
  // gold accent rule and the worldspace tagline above it.
  if (t.title_alpha > 0.003f && !t.title.empty()) {
    const f32 ts = H * 0.062f;
    const f32 ss = H * 0.024f;
    const f32 x = W * 0.055f;
    const f32 y = H - bar - H * 0.05f - ts;
    const int a = static_cast<int>(t.title_alpha * 255.0f);
    dl->AddRectFilled({x, y + ts * 0.10f}, {x + W * 0.006f, y + ts * 1.02f},
                      (kGold & 0x00FFFFFFu) | (static_cast<ImU32>(a) << 24));
    const f32 tx = x + W * 0.018f;
    if (!t.subtitle.empty())
      draw_text(ss, {tx, y - ss * 1.15f}, kGold, t.title_alpha, t.subtitle.c_str());
    draw_text(ts, {tx, y}, kWhite, t.title_alpha, t.title.c_str());
  }

  // Render-mode badge, top-left below the top bar, with the live weather under it.
  if (t.badge_alpha > 0.003f && !t.badge.empty()) {
    const f32 bs = H * 0.024f;
    const f32 ws = H * 0.018f;
    const f32 x = W * 0.055f;
    const f32 y = bar + H * 0.045f;
    draw_text(bs, {x, y}, kGold, t.badge_alpha, t.badge.c_str());
    if (!t.weather_tag.empty())
      draw_text(ws, {x, y + bs * 1.3f}, kWhite, t.badge_alpha * 0.85f, t.weather_tag.c_str());
  }

  // Loading screen: while a game streams in, a centered "LOADING <game>" over the
  // black wash, with animated trailing dots so it never looks frozen.
  if (t.loading) {
    const f32 ls = H * 0.030f;
    const std::string base =
        t.loading_label.empty() ? std::string("LOADING") : ("LOADING   " + t.loading_label);
    const ImVec2 bsz = measure(ls, base.c_str());
    const f32 lx = (W - bsz.x) * 0.5f;
    const f32 ly = H * 0.5f - ls * 0.5f;
    draw_text(ls, {lx, ly}, kWhite, 1.0f, base.c_str());
    const int n = static_cast<int>(ImGui::GetTime() * 2.0) % 4;
    if (n > 0) draw_text(ls, {lx + bsz.x, ly}, kGold, 1.0f, std::string(static_cast<size_t>(n), '.').c_str());
  }
}

void DebugUi::ScanPresetFiles() {
  preset_files_.clear();
  preset_files_scanned_ = true;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(PresetDir(), ec)) {
    if (entry.is_regular_file(ec) && entry.path().extension() == ".ini")
      preset_files_.push_back(entry.path().filename().string());
  }
  std::sort(preset_files_.begin(), preset_files_.end());
  if (preset_file_choice_ >= static_cast<int>(preset_files_.size())) preset_file_choice_ = 0;
}

void DebugUi::RenderQuestPanel(QuestPanel* quests) {
  int running = 0;
  for (const auto& q : quests->quests) running += q.running ? 1 : 0;
  ImGui::Text("%zu quests, %d running", quests->quests.size(), running);
  ImGui::TextDisabled("Papyrus quest scripts attached from VMAD");

  // NPC follow: toggle the reference the player is looking at as a follower.
  if (quests->look_target != 0 && quests->set_follower) {
    ImGui::TextUnformatted(quests->look_label.empty() ? "(look target)" : quests->look_label.c_str());
    ImGui::SameLine();
    if (quests->look_following) {
      if (ImGui::SmallButton("Stop following")) quests->set_follower(quests->look_target, false);
    } else if (ImGui::SmallButton("Follow me")) {
      quests->set_follower(quests->look_target, true);
    }
  }
  if (quests->follower_count > 0 || quests->marker_count > 0)
    ImGui::TextDisabled("%d follower(s), %d objective marker(s)", quests->follower_count,
                        quests->marker_count);

  // Case-insensitive name/id filter (1000+ quests load).
  static char filter[64] = "";
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##questfilter", "filter quests by name", filter, sizeof(filter));
  std::string needle = filter;
  std::transform(needle.begin(), needle.end(), needle.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  if (ImGui::BeginTable("quest_list", 3,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_SizingStretchProp,
                        {0, 200})) {
    ImGui::TableSetupColumn("Quest", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 80);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60);
    for (const QuestPanel::Quest& q : quests->quests) {
      if (!needle.empty()) {
        std::string name = q.name;
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (name.find(needle) == std::string::npos) continue;
      }
      ImGui::TableNextRow();
      ImGui::PushID(static_cast<int>(q.handle));
      ImGui::TableNextColumn();
      bool selected = quests->selected == q.handle;
      if (ImGui::Selectable(q.name.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
        quests->selected = q.handle;
      ImGui::TableNextColumn();
      if (q.complete)
        ImGui::TextColored({0.5f, 0.8f, 1.0f, 1}, "done %d", q.stage);
      else if (q.running)
        ImGui::TextColored({0.4f, 0.9f, 0.4f, 1}, "stage %d", q.stage);
      else
        ImGui::TextDisabled("stopped");
      ImGui::TableNextColumn();
      if (q.running) {
        if (ImGui::SmallButton("Stop") && quests->set_running) quests->set_running(q.handle, false);
      } else if (ImGui::SmallButton("Start") && quests->set_running) {
        quests->set_running(q.handle, true);
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  // Selected-quest debugger: drive its stages and objectives directly.
  const QuestPanel::Detail& d = quests->detail;
  if (quests->selected == 0 || d.handle != quests->selected) return;
  const QuestPanel::Quest* q = nullptr;
  for (const QuestPanel::Quest& it : quests->quests)
    if (it.handle == quests->selected) q = &it;
  ImGui::Separator();
  if (q) ImGui::Text("%s", q->name.c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("%s  0x%llx", d.editor_id.c_str(),
                      static_cast<unsigned long long>(d.handle));

  ImGui::PushID("detail");
  if (q && q->running) {
    if (ImGui::SmallButton("Stop") && quests->set_running) quests->set_running(d.handle, false);
  } else if (ImGui::SmallButton("Start") && quests->set_running) {
    quests->set_running(d.handle, true);
  }
  ImGui::SameLine();
  // The set-stage input is per-quest: reset it to the current stage whenever the
  // expanded quest changes so a stale value never leaks across selections.
  if (quest_stage_input_handle_ != d.handle) {
    quest_stage_input_handle_ = d.handle;
    quest_stage_input_ = q ? q->stage : 0;
  }
  ImGui::SetNextItemWidth(70);
  ImGui::InputInt("##setstage", &quest_stage_input_, 0, 0);
  ImGui::SameLine();
  if (ImGui::SmallButton("Set stage") && quests->set_stage)
    quests->set_stage(d.handle, quest_stage_input_);
  ImGui::SameLine();
  // Skip to the nearest defined stage strictly below the current one.
  if (ImGui::SmallButton("Prev") && q && quests->set_stage) {
    i32 prev = -1;
    for (const QuestPanel::Stage& s : d.stages)
      if (s.index < q->stage && s.index > prev) prev = s.index;
    if (prev >= 0) quests->set_stage(d.handle, prev);
  }
  ImGui::SameLine();
  // Skip to the next defined stage above the current one.
  if (ImGui::SmallButton("Next") && q && quests->set_stage) {
    i32 next = -1;
    for (const QuestPanel::Stage& s : d.stages)
      if (s.index > q->stage && (next < 0 || s.index < next)) next = s.index;
    if (next >= 0) quests->set_stage(d.handle, next);
  }
  if (d.completion_stage >= 0) {
    ImGui::SameLine();
    if (ImGui::SmallButton("Complete") && quests->set_stage)
      quests->set_stage(d.handle, d.completion_stage);
  }

  // Objective waypoint authoring: drop a marker at the player for the current
  // objective; reaching it advances the quest to the entered stage. The pair of
  // "set marker, set the stage it advances to" lets you wire a quest's path
  // in-world without scripts.
  if (quests->place_marker) {
    int current_obj = -1;
    for (const QuestPanel::Objective& o : d.objectives)
      if (o.displayed && !o.completed) {
        current_obj = o.index;
        break;
      }
    static int marker_stage = 0;
    ImGui::SetNextItemWidth(70);
    ImGui::InputInt("##markerstage", &marker_stage, 0, 0);
    ImGui::SameLine();
    ImGui::BeginDisabled(current_obj < 0);
    if (ImGui::SmallButton("Place marker @ player"))
      quests->place_marker(d.handle, current_obj, marker_stage);
    ImGui::EndDisabled();
    if (quests->clear_markers) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Clear")) quests->clear_markers();
    }
    if (current_obj >= 0)
      ImGui::TextDisabled("objective %d -> reaching advances to stage %d", current_obj, marker_stage);
    else
      ImGui::TextDisabled("no current objective to mark");
  }

  if (!d.stages.empty() && ImGui::TreeNode("Stages")) {
    for (const QuestPanel::Stage& s : d.stages) {
      ImGui::PushID(s.index);
      if (ImGui::SmallButton("Set") && quests->set_stage) quests->set_stage(d.handle, s.index);
      ImGui::SameLine();
      bool current = q && s.index == q->stage;
      ImVec4 col = current ? ImVec4{0.4f, 0.9f, 0.4f, 1}
                           : (s.done ? ImVec4{0.7f, 0.7f, 0.7f, 1} : ImVec4{0.5f, 0.5f, 0.5f, 1});
      ImGui::TextColored(col, "%d", s.index);
      if (!s.log.empty()) {
        ImGui::SameLine();
        ImGui::TextWrapped("%s", s.log.c_str());
      }
      ImGui::PopID();
    }
    ImGui::TreePop();
  }

  if (!d.objectives.empty() && ImGui::TreeNode("Objectives")) {
    for (const QuestPanel::Objective& o : d.objectives) {
      ImGui::PushID(o.index);
      bool displayed = o.displayed;
      if (ImGui::Checkbox("show", &displayed) && quests->set_objective_displayed)
        quests->set_objective_displayed(d.handle, o.index, displayed);
      ImGui::SameLine();
      bool completed = o.completed;
      if (ImGui::Checkbox("done", &completed) && quests->set_objective_completed)
        quests->set_objective_completed(d.handle, o.index, completed);
      ImGui::SameLine();
      ImGui::TextWrapped("%d. %s", o.index, o.text.c_str());
      ImGui::PopID();
    }
    ImGui::TreePop();
  }
  ImGui::PopID();
}

}  // namespace rec

#else  // !RECREATION_HAS_IMGUI

namespace rec {

DebugUi::DebugUi() = default;
DebugUi::~DebugUi() = default;
bool DebugUi::Initialize(Window&, render::Renderer&) { return false; }
void DebugUi::Shutdown() {}
void DebugUi::BeginFrame() {}
void DebugUi::Build(render::Renderer&, FlyCamera&, f32, render::FrameView*, QuestPanel*,
                    NativeTracePanel*) {}
bool DebugUi::wants_mouse() const { return false; }
bool DebugUi::wants_keyboard() const { return false; }

}  // namespace rec

#endif  // RECREATION_HAS_IMGUI
