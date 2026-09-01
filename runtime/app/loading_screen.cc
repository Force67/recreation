#include <chrono>

#include <base/algorithm.h>
#include <base/option.h>
#include <base/strings/to_string.h>
#include <base/strings/xstring.h>

#include "core/log.h"
#include "render/core/renderer.h"
#include "runtime/app/engine.h"
#include "runtime/app/engine_internal.h"

// The loading screen shown while a universe is brought online.
//
// This exists because of a measurement, not a whim: PLAY on the front screen
// runs LoadGameData straight through inside one Engine::OnUpdate, and on this
// box with Skyrim, Fallout 4 and Starfield mounted that call took 63 seconds.
// For all 63 the host loop never came back round, so no frame was ever
// presented: the window sat frozen on the menu it had just closed and the
// compositor marked the app unresponsive. The first thing a new player saw the
// engine do was hang.
//
// The fix is not to make the load asynchronous (that is a much larger job and
// buys nothing a player can see); it is to let the load draw. Each phase
// reports itself here, and each report pumps the window and presents one frame.
// The screen therefore animates in step with the work rather than on a timer,
// so a phase that takes twenty seconds looks like a phase that takes twenty
// seconds instead of a stuck progress bar.
namespace rx {

// Test hook: RX_UI_SHOT cannot reach this screen (it counts host frames, and
// the host loop is exactly what is not running during a load), so the loading
// screen gets its own. RX_LOAD_SHOT=<path> captures one frame of it and
// RX_LOAD_SHOT_PHASE picks which phase to catch it on. Namespace scope so it
// registers before InitOptionsFromEnv() runs at startup.
static base::Option<const char*> LoadShot{"load.shot", nullptr, "RX_LOAD_SHOT"};
static base::Option<int> LoadShotPhase{"load.shot.phase", static_cast<int>(LoadPhase::kDomains),
                                       "RX_LOAD_SHOT_PHASE"};

namespace {

// Where each phase has got to as a fraction of the whole job, from the measured
// shape of a three-universe load: reading the primary game's records is a
// tenth of it, and mounting the neighbouring worlds is nearly two thirds.
// A single-game load simply skips kDomains and jumps that span at once, which
// reads fine (the bar leaps, the phase rail explains why).
struct PhaseSpan {
  f32 begin;
  f32 end;
};
constexpr PhaseSpan kPhaseSpans[static_cast<int>(LoadPhase::kCount)] = {
    {0.00f, 0.04f},  // kArchives
    {0.04f, 0.14f},  // kRecords
    {0.14f, 0.20f},  // kText
    {0.20f, 0.30f},  // kScripts
    {0.30f, 0.92f},  // kDomains
    {0.92f, 1.00f},  // kWorld
};

f64 NowSeconds() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<f64>(clock::now().time_since_epoch()).count();
}

// Digit grouping, because "1168663 records" is a number nobody reads and
// "1,168,663 records" is one everybody does.
base::String Grouped(u64 n) {
  const base::String digits = base::ToString(n);
  base::String out;
  const mem_size size = digits.size();
  for (mem_size i = 0; i < size; ++i) {
    if (i != 0 && (size - i) % 3 == 0)
      out += ',';
    out += digits[i];
  }
  return out;
}

}  // namespace

// Draw and present one frame of the loading screen. This is the whole trick:
// the host's own loop is blocked inside the load, so the frame is submitted
// from here instead. The FrameView is function-local static so it keeps its
// vector capacity across the dozens of pumps a load makes; the load runs on the
// main thread and nowhere else.
void PresentLoadingFrame(Engine& engine) {
  Engine* const self = &engine;
  if (self->config_.headless || !self->renderer_ || !self->window_)
    return;

  // Keep the window alive while the load has the main thread: without pumping,
  // the compositor sees an app that has stopped answering and greys it out.
  // A close request during a load is honoured as soon as the load returns.
  if (!self->window_->PumpEvents()) {
    self->RequestQuit();
    return;
  }

  static render::FrameView view;
  view.Clear();
  view.frame_delta_seconds = 1.0f / 60.0f;
  // Nothing of the world is up yet, but the camera still has to be a valid
  // basis or the view matrix is degenerate.
  view.camera.eye = self->camera_.position();
  view.camera.target = self->camera_.target();
  self->game_ui_.Build(*self->window_, *self->renderer_, self->camera_, view.frame_delta_seconds,
                       &view);
  self->renderer_->RenderFrame(view);
}

void BeginLoadingScreen(Engine& engine, const base::String& title) {
  Engine* const self = &engine;
  if (self->config_.headless || !self->renderer_ || !self->window_)
    return;
  self->load_screen_up_ = true;
  self->load_started_ = NowSeconds();
  self->load_title_ = title;
  self->load_records_.clear();
  self->load_plugins_.clear();
  self->game_ui_.OpenLoading(title);
  // Two frames, not one: the first is the one that replaces the menu, and with
  // double buffering the second is what guarantees the player has actually seen
  // it before the thread disappears into the load.
  ReportLoadPhase(engine, LoadPhase::kArchives, "Opening the game's archives");
  PresentLoadingFrame(engine);
}

void ReportLoadPhase(Engine& engine,
                     LoadPhase phase,
                     const base::String& detail,
                     const base::String& note,
                     f32 within) {
  Engine* const self = &engine;
  if (!self->load_screen_up_)
    return;

  const int index = base::Clamp(static_cast<int>(phase), 0,
                                static_cast<int>(LoadPhase::kCount) - 1);
  const PhaseSpan& span = kPhaseSpans[index];

  // The counts are sticky: kRecords learns them and every later phase keeps
  // showing them rather than blanking the panel back to "-".
  if (self->records_.record_count() > 0)
    self->load_records_ = Grouped(self->records_.record_count());

  LoadingView view;
  view.title = self->load_title_;
  view.phase = detail;
  view.detail = note;
  view.records = self->load_records_;
  view.plugins = self->load_plugins_;
  view.step = index;
  view.progress = span.begin + (span.end - span.begin) * base::Clamp(within, 0.0f, 1.0f);
  view.elapsed = static_cast<f32>(NowSeconds() - self->load_started_);
  self->game_ui_.SetLoadingView(view);
  PresentLoadingFrame(engine);

  // Test hook: grab this screen once, on the phase asked for. CaptureScreenshot
  // is deferred to the NEXT RenderFrame, so the request is followed by another
  // present to make it land.
  if (const char* shot = LoadShot.get(); shot && *shot && index == LoadShotPhase.get()) {
    static bool taken = false;
    if (!taken) {
      taken = true;
      self->renderer_->CaptureScreenshot(shot);
      PresentLoadingFrame(engine);
      RX_INFO("loading screen captured to {}", shot);
    }
  }
}

void EndLoadingScreen(Engine& engine) {
  Engine* const self = &engine;
  if (!self->load_screen_up_)
    return;
  self->load_screen_up_ = false;
  self->game_ui_.CloseLoading();
  RX_INFO("loaded {} in {:.1f}s", self->load_title_, NowSeconds() - self->load_started_);
}

}  // namespace rx
