#include "debug_ui.h"

#include "fly_camera.h"

#if defined(RECREATION_HAS_IMGUI)

#include <algorithm>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include "core/log.h"

namespace rec {
namespace {

const char* kAaModes[] = {"None", "TAA", "FSR3 Upscaler", "DLSS Upscaler"};
const char* kQualities[] = {"Native AA (1.0x)", "Quality (1.5x)", "Balanced (1.7x)",
                            "Performance (2.0x)"};
const char* kTonemaps[] = {"ACES", "Reinhard", "None"};
const char* kDebugViews[] = {"Off",         "Base color",   "World normal",
                             "Roughness",   "Metallic",     "Ambient occlusion",
                             "Indirect GI", "Direct light", "Emissive", "Reflection"};

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
                    render::FrameView* view, QuestPanel* quests) {
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
      ImGui::Text("%.2f ms (%.0f fps)", frame_delta * 1000.0f,
                  frame_delta > 0 ? 1.0f / frame_delta : 0.0f);
      ImGui::PlotLines("##frametimes", frame_times_, IM_ARRAYSIZE(frame_times_),
                       static_cast<int>(frame_time_cursor_), nullptr, 0.0f, 33.3f,
                       {ImGui::GetContentRegionAvail().x, 48});

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
        ImGui::Checkbox("Water RT reflections", &settings.water_reflections);
        ImGui::Checkbox("RT reflections", &settings.rt_reflections);
        if (settings.rt_reflections) {
          ImGui::SliderFloat("Reflection roughness", &settings.reflection_roughness_cutoff, 0.05f,
                             1.0f, "%.2f");
        }
        ImGui::Checkbox("Path traced reference", &settings.path_trace);
        if (settings.path_trace) {
          ImGui::Text("accumulated %u spp", renderer.path_trace_samples());
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
        if (settings.rtao) {
          ImGui::SliderFloat("AO radius", &settings.ao_radius, 0.2f, 5.0f);
          ImGui::SliderFloat("AO intensity", &settings.ao_intensity, 0.2f, 3.0f);
          int rays = static_cast<int>(settings.ao_rays);
          if (ImGui::SliderInt("AO rays", &rays, 1, 8)) settings.ao_rays = rays;
        }
        ImGui::EndDisabled();
      }

      if (ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
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

      if (ImGui::CollapsingHeader("Post processing", ImGuiTreeNodeFlags_DefaultOpen)) {
        int tonemap = static_cast<int>(settings.tonemap);
        if (ImGui::Combo("Tonemap", &tonemap, kTonemaps, IM_ARRAYSIZE(kTonemaps))) {
          settings.tonemap = static_cast<render::TonemapOperator>(tonemap);
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

      if (quests && quests->available &&
          ImGui::CollapsingHeader("Quests", ImGuiTreeNodeFlags_DefaultOpen)) {
        int running = 0;
        for (const auto& q : quests->quests) running += q.running ? 1 : 0;
        ImGui::Text("%zu quests, %d running", quests->quests.size(), running);
        ImGui::TextDisabled("Papyrus quest scripts attached from VMAD");
        if (ImGui::BeginTable("quest_list", 3,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingStretchProp,
                              {0, 240})) {
          ImGui::TableSetupColumn("Quest", ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 90);
          ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 120);
          for (const QuestPanel::Quest& q : quests->quests) {
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(q.handle));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(q.name.c_str());
            ImGui::TableNextColumn();
            if (q.running)
              ImGui::TextColored({0.4f, 0.9f, 0.4f, 1}, "stage %d", q.stage);
            else
              ImGui::TextDisabled("stopped");
            ImGui::TableNextColumn();
            if (q.running) {
              if (ImGui::SmallButton("Stop") && quests->set_running)
                quests->set_running(q.handle, false);
              ImGui::SameLine();
              int stage = q.stage;
              ImGui::SetNextItemWidth(60);
              if (ImGui::InputInt("##stage", &stage, 0, 0,
                                  ImGuiInputTextFlags_EnterReturnsTrue) &&
                  quests->set_stage)
                quests->set_stage(q.handle, stage);
            } else if (ImGui::SmallButton("Start") && quests->set_running) {
              quests->set_running(q.handle, true);
            }
            ImGui::PopID();
          }
          ImGui::EndTable();
        }
      }
    }
    ImGui::End();

    if (show_demo_) ImGui::ShowDemoWindow(&show_demo_);
  }

  ImGui::Render();
  if (visible_ || ImGui::GetDrawData()->TotalVtxCount > 0) {
    view->ui_draw = [](VkCommandBuffer cmd) {
      ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    };
  }
}

}  // namespace rec

#else  // !RECREATION_HAS_IMGUI

namespace rec {

DebugUi::DebugUi() = default;
DebugUi::~DebugUi() = default;
bool DebugUi::Initialize(Window&, render::Renderer&) { return false; }
void DebugUi::Shutdown() {}
void DebugUi::BeginFrame() {}
void DebugUi::Build(render::Renderer&, FlyCamera&, f32, render::FrameView*, QuestPanel*) {}
bool DebugUi::wants_mouse() const { return false; }
bool DebugUi::wants_keyboard() const { return false; }

}  // namespace rec

#endif  // RECREATION_HAS_IMGUI
