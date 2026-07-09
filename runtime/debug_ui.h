#ifndef RECREATION_RUNTIME_DEBUG_UI_H_
#define RECREATION_RUNTIME_DEBUG_UI_H_

// The overlay records raw Vulkan (imgui_impl_vulkan); the rhi headers pulled in
// via renderer.h are backend-agnostic, so the VkFormat member below needs volk
// directly (cf. gui_backend.h).
#include <volk.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/types.h"
#include "core/window.h"
#include "core/world_clock.h"
#include "render/core/renderer.h"
#include "trailer.h"
#include "weather/weather.h"

// Dear ImGui's font handle, forward-declared so this header stays free of imgui
// (it compiles to a stub when imgui is absent).
struct ImFont;

namespace rx {

class FlyCamera;

// Live quest state the engine feeds the debug overlay so it can list quests and
// drive them. The engine fills `quests` from a throttled snapshot of the guest's
// quest state and wires the callbacks to the guest thread; the overlay only
// reads the snapshot and invokes the callbacks. handle is the quest's packed
// form id (its papyrus instance handle).
struct QuestPanel {
  struct Quest {
    std::string name;
    u64 handle = 0;
    bool running = false;
    bool active = true;
    bool complete = false;
    i32 stage = 0;
  };
  // One journal stage of the selected quest, with the engine's done flag.
  struct Stage {
    i32 index = 0;
    std::string log;
    bool done = false;
  };
  // One objective of the selected quest, with its live display state.
  struct Objective {
    i32 index = 0;
    std::string text;
    bool displayed = false;
    bool completed = false;
  };
  // Full breakdown of the selected quest, filled by the engine for `selected`.
  struct Detail {
    u64 handle = 0;  // matches `selected` once the engine has filled it
    std::string editor_id;
    i32 completion_stage = -1;  // stage that completes the quest, -1 if none
    std::vector<Stage> stages;
    std::vector<Objective> objectives;
  };
  bool available = false;
  std::vector<Quest> quests;
  u64 selected = 0;  // UI -> engine: which quest to expand in `detail`
  Detail detail;     // engine -> UI: breakdown of `selected`
  std::function<void(u64 handle, bool run)> set_running;
  std::function<void(u64 handle, i32 stage)> set_stage;
  std::function<void(u64 handle, i32 objective, bool displayed)> set_objective_displayed;
  std::function<void(u64 handle, i32 objective, bool completed)> set_objective_completed;

  // NPC follow control: the reference the player is currently looking at (0 =
  // none), its HUD label, and whether it is already a follower. The button wires
  // to the engine's follower registry. follower_count is shown live.
  u64 look_target = 0;
  std::string look_label;
  bool look_following = false;
  int follower_count = 0;
  std::function<void(u64 npc, bool follow)> set_follower;

  // Objective waypoint authoring: drop a marker at the player for one quest
  // objective (reaching it advances the quest to advance_stage), or clear them
  // all. marker_count is the number currently authored.
  int marker_count = 0;
  std::function<void(u64 quest, i32 objective, i32 advance_stage)> place_marker;
  std::function<void()> clear_markers;
};

// Recently invoked Papyrus native functions, for the trace window. The engine
// snapshots the guest VM's native-call ring here while the window is open and
// wires Clear back to the guest thread.
struct NativeTracePanel {
  bool available = false;
  u64 total = 0;                          // native calls since tracing began
  std::vector<std::string> recent;        // "Type.Function", newest first
  std::vector<std::pair<std::string, u32>> top;  // name -> count, busiest first
  std::function<void()> clear;
};

// Dear ImGui overlay: frame stats plus live toggles for every render
// feature. Rendered through the renderer's ui pass straight onto the
// backbuffer. Compiles to a stub when imgui/SDL3 are unavailable.
class DebugUi {
 public:
  DebugUi();
  ~DebugUi();

  DebugUi(const DebugUi&) = delete;
  DebugUi& operator=(const DebugUi&) = delete;

  bool Initialize(Window& window, render::Renderer& renderer);
  // Call between renderer WaitIdle and renderer Shutdown.
  void Shutdown();

  // Starts the imgui frame; call once per frame after PumpEvents.
  void BeginFrame();
  // Builds the panels and fills view->ui_draw. Always pairs with a
  // BeginFrame, even while hidden.
  void Build(render::Renderer& renderer, FlyCamera& camera, f32 frame_delta,
             render::FrameView* view, QuestPanel* quests = nullptr,
             NativeTracePanel* trace = nullptr);

  // The day/night clock, so the Lighting panel can scrub the time of day and the
  // timescale. Null leaves those controls out.
  void set_clock(WorldClock* clock) { clock_ = clock; }
  // The engine's weather override: when `*enable` is set, the loop uses `*state`
  // instead of the climate, so the Weather panel can drive the sky live.
  void set_weather(bool* enable, weather::WeatherState* state) {
    weather_enable_ = enable;
    weather_state_ = state;
  }
  // The trailer's per-frame chrome (letterbox, fades, title cards, render-mode
  // badge), drawn onto the foreground draw list. Null disables it.
  void SetTrailerOverlay(const TrailerOverlay* overlay) { trailer_ = overlay; }

  void ToggleVisible() { visible_ = !visible_; }
  void SetVisible(bool v) { visible_ = v; }
  // Hide/show every overlay window at once (renderer, trace, quests), for clean
  // captures.
  void SetAllVisible(bool v) { visible_ = trace_visible_ = quests_visible_ = v; }
  void ToggleTrace() { trace_visible_ = !trace_visible_; }
  void ToggleQuests() { quests_visible_ = !quests_visible_; }
  bool trace_visible() const { return trace_visible_; }
  bool wants_mouse() const;
  bool wants_keyboard() const;

 private:
  // Renders the quest debugger body (list + selected-quest stage/objective
  // controls) into the current window.
  void RenderQuestPanel(QuestPanel* quests);

  // Per-topic bodies of the render-settings window's tab bar. Each draws one
  // area of the pipeline; Build() wraps them in tab items so the long flat list
  // of options is split into navigable submenus.
  void DrawDisplayTab(render::Renderer& renderer, render::RenderSettings& settings);
  void DrawRayTracingTab(render::Renderer& renderer, render::RenderSettings& settings,
                         const render::DeviceCaps* caps);
  void DrawLightingTab(render::RenderSettings& settings, const render::DeviceCaps* caps);
  void DrawGiTab(render::RenderSettings& settings, const render::DeviceCaps* caps);
  void DrawPostTab(render::RenderSettings& settings);
  void DrawDiagnosticsTab(render::Renderer& renderer, FlyCamera& camera,
                          render::RenderSettings& settings, const render::DeviceCaps* caps);

  // Refreshes preset_files_ from the .ini files in the presets directory.
  void ScanPresetFiles();

  // Window-less GPU-stage bar chart pinned to the bottom-left, drawn straight
  // onto the foreground draw list from the renderer's per-pass timings.
  void DrawStageChart(render::Renderer& renderer);

  // Cinematic trailer chrome (RX_TRAILER) onto the foreground draw list:
  // letterbox bars, the intro/outro black wash, the lower-third location title
  // and the render-mode badge. A no-op unless an overlay is set and active.
  void DrawTrailerOverlay();

  bool initialized_ = false;
  bool visible_ = true;
  Window* window_ = nullptr;  // for the live system-HDR state in the Display tab
  bool trace_visible_ = true;   // the native-call trace window (F2 toggles)
  bool quests_visible_ = true;  // the quest debugger window (F3 toggles)
  // Per-pass GPU timestamps follow overlay visibility; the boot value
  // (RX_GPU_TIMINGS / preset) is latched so a forced run keeps them.
  bool gpu_timings_latched_ = false;
  bool gpu_timings_forced_ = false;
  bool show_demo_ = false;
  WorldClock* clock_ = nullptr;  // day/night cycle, for the Lighting time controls
  bool* weather_enable_ = nullptr;  // engine weather-override flag + state, for the Weather panel
  weather::WeatherState* weather_state_ = nullptr;
  const TrailerOverlay* trailer_ = nullptr;  // cinematic trailer chrome, when running
  ImFont* title_font_ = nullptr;  // large face for trailer titles (null = default, scaled)
  int preset_choice_ = 0;  // 0 = custom/hand-tuned, else a QualityPreset combo row
  // Editable .ini render presets (engine/render/presets): the discovered file
  // list (lazy-scanned, rescannable), the combo selection, the save-as name
  // buffer and the last load/save status line.
  std::vector<std::string> preset_files_;
  bool preset_files_scanned_ = false;
  int preset_file_choice_ = 0;
  char preset_save_name_[64] = "custom";
  std::string preset_status_;
  // The set-stage InputInt tracks the selected quest: switching selection resets
  // it instead of carrying a stale stage from the previously expanded quest.
  u64 quest_stage_input_handle_ = 0;
  int quest_stage_input_ = 0;
  VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;  // outlives imgui init info
  f32 render_scale_ui_ = 1.0f;  // in-progress render-scale slider; committed on release
  f32 frame_times_[150] = {};
  u32 frame_time_cursor_ = 0;
};

}  // namespace rx

#endif  // RECREATION_RUNTIME_DEBUG_UI_H_
