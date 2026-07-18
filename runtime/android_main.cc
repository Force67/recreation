// Android entry point. A NativeActivity hands us an ANativeWindow through the
// native-glue lifecycle; we build an EngineConfig from the file the Jetpack
// Compose launcher wrote, then drive Engine::RunFrame() from the glue's event
// loop (Android owns the loop, so we never call the blocking Engine::Run()).
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>

#include "bethesda/game_profile.h"
#include "core/log.h"
#include "core/window.h"
#include "engine.h"
#include "render/core/presets.h"

namespace {

constexpr const char* kTag = "recreation";

rx::bethesda::Game ParseGame(const std::string& id) {
  if (id == "skyrimse") return rx::bethesda::Game::kSkyrimSe;
  if (id == "fo4") return rx::bethesda::Game::kFallout4;
  if (id == "fo76") return rx::bethesda::Game::kFallout76;
  if (id == "starfield") return rx::bethesda::Game::kStarfield;
  if (id == "fo3") return rx::bethesda::Game::kFallout3;
  if (id == "fnv") return rx::bethesda::Game::kFalloutNv;
  return rx::bethesda::Game::kUnknown;
}

// Reads the launcher's config (`recreation.cfg` in the app's internal data
// dir). Format is one `key=value` per line; unknown keys are ignored. The UI
// stores every configured game's Data path under `data_dir.<id>` and selects
// one with `active`, so several games can be configured while one is launched.
rx::EngineConfig LoadConfig(android_app* app) {
  rx::EngineConfig config;
  config.preset = rx::render::QualityPreset::kAndroid;

  std::string path = std::string(app->activity->internalDataPath) + "/recreation.cfg";
  std::ifstream file(path);
  if (!file) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "no config at %s, using demo scene", path.c_str());
    return config;
  }

  std::unordered_map<std::string, std::string> kv;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = line.substr(0, eq);
    std::string value = line.substr(eq + 1);
    while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
    kv[key] = value;
  }

  auto get = [&](const std::string& key) -> std::string {
    auto it = kv.find(key);
    return it == kv.end() ? std::string() : it->second;
  };

  std::string active = get("active");
  if (!active.empty()) {
    config.game = ParseGame(active);
    config.data_dir = get("data_dir." + active);
    if (!config.data_dir.empty()) {
      config.plugins_txt = config.data_dir + "/../plugins.txt";
    }
  }
  std::string gltf = get("gltf");
  if (!gltf.empty()) config.gltf_path = gltf;
  std::string demo = get("demo");
  if (!demo.empty()) config.demo_scene = demo;
  std::string preset = get("preset");
  if (!preset.empty()) config.preset = rx::render::ParsePreset(preset);
  std::string interior = get("interior");
  if (!interior.empty()) config.interior = interior;
  if (get("validation") == "1") config.renderer.enable_validation = true;
  if (get("no_rt") == "1") config.renderer.enable_raytracing = false;
  // screenshot=<seconds>: the renderer reads RX_SCREENSHOT and writes the
  // frame at that time to the app's data dir, for on-device render verification
  // that does not depend on the platform screenshotter.
  std::string shot = get("screenshot");
  if (!shot.empty()) {
    std::string shot_path = std::string(app->activity->internalDataPath) + "/frame.png:" + shot;
    setenv("RX_SCREENSHOT", shot_path.c_str(), 1);
  }

  __android_log_print(ANDROID_LOG_INFO, kTag, "config: game=%s data_dir=%s", active.c_str(),
                      config.data_dir.c_str());
  return config;
}

struct AppState {
  // engine before host so the host (which holds a pointer into the game) tears
  // down first.
  std::unique_ptr<rx::Engine> engine;    // the game, built once the config is known
  rx::app::Host host;                    // owns the subsystems + the loop
  rx::AndroidWindowBase* window = nullptr;  // owned by the host post-Initialize
  rx::EngineConfig config;
  bool initialized = false;
  bool finished = false;
  bool has_surface = false;  // window present and surface bound (between INIT/TERM)
  // Touch controls: a pointer that goes down in the left half drives a virtual
  // movement stick (mapped to WASD), one in the right half looks around (mapped
  // to a mouse-look drag). Each control is owned by a single pointer id.
  int move_pointer = -1;
  float move_origin_x = 0;
  float move_origin_y = 0;
  int look_pointer = -1;
  float look_last_x = 0;
  float look_last_y = 0;
};

void SetKey(rx::InputState& input, rx::Key key, bool down) {
  input.keys[static_cast<rx::u8>(key)] = down;
}

// Maps the virtual stick offset (pixels from where the finger went down) to the
// fly camera's movement keys, with a small deadzone.
void ApplyMoveStick(rx::InputState& input, float dx, float dy) {
  constexpr float kDeadzone = 36.0f;
  SetKey(input, rx::Key::kW, dy < -kDeadzone);
  SetKey(input, rx::Key::kS, dy > kDeadzone);
  SetKey(input, rx::Key::kA, dx < -kDeadzone);
  SetKey(input, rx::Key::kD, dx > kDeadzone);
}

void ClearMoveStick(rx::InputState& input) {
  SetKey(input, rx::Key::kW, false);
  SetKey(input, rx::Key::kS, false);
  SetKey(input, rx::Key::kA, false);
  SetKey(input, rx::Key::kD, false);
}

void HandleCmd(android_app* app, int32_t cmd) {
  auto* state = static_cast<AppState*>(app->userData);
  switch (cmd) {
    case APP_CMD_INIT_WINDOW:
      if (app->window == nullptr) break;
      if (!state->initialized) {
        auto window = rx::CreateAndroidWindow(app->window);  // acquires the window
        state->window = window.get();
        rx::app::AppConfig app_config;
        app_config.renderer = state->config.renderer;
        app_config.preset = state->config.preset;
        app_config.headless = state->config.headless;
        app_config.gather_entity_draws = false;  // the game gathers its own draws
        state->engine = std::make_unique<rx::Engine>(state->config);
        if (!state->host.Initialize(app_config, *state->engine, std::move(window))) {
          __android_log_print(ANDROID_LOG_ERROR, kTag, "engine initialization failed");
          state->finished = true;
          ANativeActivity_finish(app->activity);
          return;
        }
        state->initialized = true;
        state->has_surface = true;
        __android_log_print(ANDROID_LOG_INFO, kTag, "engine initialized");
      } else {
        // Foregrounded: rebind the renderer to the new activity window.
        state->window->SetNativeWindow(app->window);
        state->host.OnSurfaceCreated();
        state->has_surface = true;
        __android_log_print(ANDROID_LOG_INFO, kTag, "surface recreated");
      }
      break;
    case APP_CMD_TERM_WINDOW:
      // Backgrounded or rotated: drop the surface but keep the engine alive so
      // it resumes on the next INIT_WINDOW.
      if (state->initialized) {
        state->host.OnSurfaceDestroyed();
        state->window->SetNativeWindow(nullptr);
        state->has_surface = false;
      }
      break;
    case APP_CMD_DESTROY:
      state->finished = true;
      if (state->initialized) state->window->RequestQuit();
      break;
    default:
      break;
  }
}

void PointerDown(AppState* state, rx::InputState& input, int id, float x, float y, float mid_x) {
  if (x < mid_x) {
    if (state->move_pointer < 0) {
      state->move_pointer = id;
      state->move_origin_x = x;
      state->move_origin_y = y;
    }
  } else if (state->look_pointer < 0) {
    state->look_pointer = id;
    state->look_last_x = x;
    state->look_last_y = y;
    input.mouse[static_cast<rx::u8>(rx::MouseButton::kRight)] = true;
  }
}

void PointerUp(AppState* state, rx::InputState& input, int id) {
  if (id == state->move_pointer) {
    state->move_pointer = -1;
    ClearMoveStick(input);
  } else if (id == state->look_pointer) {
    state->look_pointer = -1;
    input.mouse[static_cast<rx::u8>(rx::MouseButton::kRight)] = false;
  }
}

int32_t HandleInput(android_app* app, AInputEvent* event) {
  auto* state = static_cast<AppState*>(app->userData);
  if (!state->initialized || state->window == nullptr) return 0;
  if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) return 0;

  rx::InputState& input = state->window->mutable_input();
  // Touch x arrives in the activity's (landscape) space; the larger dimension is
  // the horizontal extent, so split the zones at half of it.
  const float mid_x =
      0.5f * static_cast<float>(std::max(state->window->width(), state->window->height()));

  const int32_t action = AMotionEvent_getAction(event);
  const int32_t masked = action & AMOTION_EVENT_ACTION_MASK;
  switch (masked) {
    case AMOTION_EVENT_ACTION_DOWN:
    case AMOTION_EVENT_ACTION_POINTER_DOWN: {
      const int32_t index = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                            AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
      PointerDown(state, input, AMotionEvent_getPointerId(event, index),
                  AMotionEvent_getX(event, index), AMotionEvent_getY(event, index), mid_x);
      break;
    }
    case AMOTION_EVENT_ACTION_MOVE: {
      const size_t count = AMotionEvent_getPointerCount(event);
      for (size_t i = 0; i < count; ++i) {
        const int id = AMotionEvent_getPointerId(event, i);
        const float x = AMotionEvent_getX(event, i);
        const float y = AMotionEvent_getY(event, i);
        if (id == state->move_pointer) {
          ApplyMoveStick(input, x - state->move_origin_x, y - state->move_origin_y);
        } else if (id == state->look_pointer) {
          input.mouse_dx += x - state->look_last_x;
          input.mouse_dy += y - state->look_last_y;
          state->look_last_x = x;
          state->look_last_y = y;
        }
      }
      break;
    }
    case AMOTION_EVENT_ACTION_POINTER_UP: {
      const int32_t index = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                            AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
      PointerUp(state, input, AMotionEvent_getPointerId(event, index));
      break;
    }
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_CANCEL:
      PointerUp(state, input, state->move_pointer);
      PointerUp(state, input, state->look_pointer);
      break;
    default:
      break;
  }
  return 1;
}

}  // namespace

void android_main(android_app* app) {
  AppState state;
  app->userData = &state;
  app->onAppCmd = HandleCmd;
  app->onInputEvent = HandleInput;

  state.config = LoadConfig(app);

  while (!state.finished) {
    const bool active = state.initialized && state.has_surface;
    // Look deltas are per-frame; the window backend never resets them, so clear
    // them here before this frame's motion events accumulate. Movement keys are
    // held state and persist until the stick pointer lifts.
    if (active) {
      rx::InputState& input = state.window->mutable_input();
      input.mouse_dx = 0;
      input.mouse_dy = 0;
      input.wheel = 0;
    }

    int events = 0;
    android_poll_source* source = nullptr;
    // Block for events while paused (no surface); once rendering, drain without
    // blocking so we render every frame.
    int timeout = active ? 0 : -1;
    while (ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
      if (source != nullptr) source->process(app, source);
      if (app->destroyRequested) {
        state.finished = true;
        break;
      }
      timeout = 0;
    }
    if (state.finished) break;
    if (state.initialized && state.has_surface) {
      if (!state.host.RunFrame()) break;
    }
  }

  if (state.initialized) state.host.Shutdown();
}
