// Android entry point. A NativeActivity hands us an ANativeWindow through the
// native-glue lifecycle; we build an EngineConfig from the file the Jetpack
// Compose launcher wrote, then drive Engine::RunFrame() from the glue's event
// loop (Android owns the loop, so we never call the blocking Engine::Run()).
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

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
#include "render/presets.h"

namespace {

constexpr const char* kTag = "recreation";

rec::bethesda::Game ParseGame(const std::string& id) {
  if (id == "skyrimse") return rec::bethesda::Game::kSkyrimSe;
  if (id == "fo4") return rec::bethesda::Game::kFallout4;
  if (id == "fo76") return rec::bethesda::Game::kFallout76;
  return rec::bethesda::Game::kUnknown;
}

// Reads the launcher's config (`recreation.cfg` in the app's internal data
// dir). Format is one `key=value` per line; unknown keys are ignored. The UI
// stores every configured game's Data path under `data_dir.<id>` and selects
// one with `active`, so several games can be configured while one is launched.
rec::EngineConfig LoadConfig(android_app* app) {
  rec::EngineConfig config;
  config.preset = rec::render::QualityPreset::kAndroid;

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
  if (!preset.empty()) config.preset = rec::render::ParsePreset(preset);
  std::string interior = get("interior");
  if (!interior.empty()) config.interior = interior;
  if (get("validation") == "1") config.renderer.enable_validation = true;
  if (get("no_rt") == "1") config.renderer.enable_raytracing = false;
  // screenshot=<seconds>: the renderer reads REC_SCREENSHOT and writes the
  // frame at that time to the app's data dir, for on-device render verification
  // that does not depend on the platform screenshotter.
  std::string shot = get("screenshot");
  if (!shot.empty()) {
    std::string path = std::string(app->activity->internalDataPath) + "/frame.png:" + shot;
    setenv("REC_SCREENSHOT", path.c_str(), 1);
  }

  __android_log_print(ANDROID_LOG_INFO, kTag, "config: game=%s data_dir=%s", active.c_str(),
                      config.data_dir.c_str());
  return config;
}

struct AppState {
  rec::Engine engine;
  rec::AndroidWindowBase* window = nullptr;  // owned by the engine post-Initialize
  rec::EngineConfig config;
  bool initialized = false;
  bool finished = false;
  // Last touch position, for turning drags into camera-look deltas.
  bool touching = false;
  float last_x = 0;
  float last_y = 0;
};

void HandleCmd(android_app* app, int32_t cmd) {
  auto* state = static_cast<AppState*>(app->userData);
  switch (cmd) {
    case APP_CMD_INIT_WINDOW:
      if (app->window != nullptr && !state->initialized) {
        ANativeWindow_acquire(app->window);
        auto window = rec::CreateAndroidWindow(app->window);
        state->window = window.get();
        if (!state->engine.Initialize(state->config, std::move(window))) {
          __android_log_print(ANDROID_LOG_ERROR, kTag, "engine initialization failed");
          state->finished = true;
          ANativeActivity_finish(app->activity);
          return;
        }
        state->initialized = true;
        __android_log_print(ANDROID_LOG_INFO, kTag, "engine initialized");
      }
      break;
    case APP_CMD_TERM_WINDOW:
      // The surface is being destroyed (app backgrounded or rotated). Surface
      // recreation on resume is a follow-up; for now end the frame loop.
      if (state->initialized) state->window->RequestQuit();
      break;
    case APP_CMD_DESTROY:
      state->finished = true;
      if (state->initialized) state->window->RequestQuit();
      break;
    default:
      break;
  }
}

int32_t HandleInput(android_app* app, AInputEvent* event) {
  auto* state = static_cast<AppState*>(app->userData);
  if (!state->initialized) return 0;
  if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) return 0;

  rec::InputState& input = state->window->mutable_input();
  int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
  float x = AMotionEvent_getX(event, 0);
  float y = AMotionEvent_getY(event, 0);
  switch (action) {
    case AMOTION_EVENT_ACTION_DOWN:
      state->touching = true;
      state->last_x = x;
      state->last_y = y;
      input.mouse[static_cast<rec::u8>(rec::MouseButton::kRight)] = true;
      break;
    case AMOTION_EVENT_ACTION_MOVE:
      if (state->touching) {
        input.mouse_dx += x - state->last_x;
        input.mouse_dy += y - state->last_y;
        state->last_x = x;
        state->last_y = y;
      }
      break;
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_CANCEL:
      state->touching = false;
      input.mouse[static_cast<rec::u8>(rec::MouseButton::kRight)] = false;
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
    int events = 0;
    android_poll_source* source = nullptr;
    // Block for events until the engine is up; once rendering, drain without
    // blocking so we render every frame.
    int timeout = state.initialized ? 0 : -1;
    while (ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
      if (source != nullptr) source->process(app, source);
      if (app->destroyRequested) {
        state.finished = true;
        break;
      }
      timeout = 0;
    }
    if (state.finished) break;
    if (state.initialized) {
      if (!state.engine.RunFrame()) break;
    }
  }

  if (state.initialized) state.engine.Shutdown();
}
