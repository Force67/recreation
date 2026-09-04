#include <chrono>

#include <base/algorithm.h>
#include <base/option.h>
#include <base/strings/to_string.h>
#include <base/strings/xstring.h>

#include "components/bethesda/load_screen.h"
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
// Shortest time the loading screen stays up, however fast the load was.
// RX_LOAD_MIN_SECONDS=0 turns the hold off (a capture run that wants the world
// as soon as it exists).
static base::Option<float> LoadMinimumSeconds{"load.min.seconds", 2.5f, "RX_LOAD_MIN_SECONDS"};
// Safety cap on the hold that waits for the world to stream in. A map that
// never reports caught-up (a hole in the LAND, an interior the streamer is
// still chewing) must not strand the player behind the card forever.
static base::Option<float> LoadMaxHoldSeconds{"load.max.hold", 20.0f, "RX_LOAD_MAX_HOLD"};
// The game's own loading-screen model. RX_LOAD_ART=0 turns it off, which is
// also the A/B for telling its uploads apart from the rest of the load.
static base::Option<bool> LoadArt{"load.art", true, "RX_LOAD_ART"};
// Stage dressing, tunable so the look can be swept without a rebuild.
static base::Option<float> LoadExposure{"load.exposure", 1.0f, "RX_LOAD_EXPOSURE"};
static base::Option<float> LoadLight{"load.light", 6.0f, "RX_LOAD_LIGHT"};

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

  // Real time between presents, not a nominal 1/60: the gap between two phase
  // reports is however long that phase took, and the UI clock (which paces the
  // page fades and the tip rotation) has to agree with the wall clock or the
  // screen ages at a rate that has nothing to do with the load. Clamped so one
  // twenty-second phase does not hand the UI a twenty-second step.
  static f64 last_present = 0.0;
  const f64 now = NowSeconds();
  const f32 delta =
      last_present > 0.0 ? base::Clamp(static_cast<f32>(now - last_present), 0.0f, 0.1f) : 1.0f / 60.0f;
  last_present = now;

  static render::FrameView view;
  view.Clear();
  view.frame_delta_seconds = delta;
  // Nothing of the world is up yet, but the camera still has to be a valid
  // basis or the view matrix is degenerate.
  view.camera.eye = self->camera_.position();
  view.camera.target = self->camera_.target();
  AppendLoadScreenModel(engine, view);  // overrides the camera when there is one
  self->game_ui_.Build(*self->window_, *self->renderer_, self->camera_, delta, &view);
  self->renderer_->RenderFrame(view);
}

// Where the loading model is put, in engine metres. Far under any worldspace,
// because the world keeps streaming in behind this screen during the hold and
// the two must not share a shot.
constexpr f32 kLoadStageY = -8000.0f;

void PickLoadScreenArt(Engine& engine) {
  Engine* const self = &engine;
  self->load_model_mesh_ = 0;
  self->load_model_text_.clear();
  if (self->config_.headless || !self->renderer_ || !self->assets_ || !LoadArt)
    return;

  base::Vector<bethesda::LoadScreen> screens;
  if (bethesda::LoadLoadScreens(self->records_, &screens) == 0)
    return;  // a game that authors none; the screen just shows no model

  // One at random, then walk on if its mesh will not load. Bethesda's own
  // screens are picked at random too (filtered by conditions this has no save
  // to evaluate), so the shuffle is the authentic behaviour rather than a
  // shortcut.
  const int count = static_cast<int>(screens.size());
  int index = static_cast<int>(NowSeconds() * 1000.0) % count;
  for (int tried = 0; tried < count; ++tried, index = (index + 1) % count) {
    const bethesda::LoadScreen& screen = screens[index];
    const base::String path = bethesda::LoadScreenModelPath(self->records_, screen);
    if (path.empty())
      continue;
    const asset::Mesh* mesh = self->assets_->LoadMesh(path);
    if (!mesh || mesh->lods.empty())
      continue;

    // Materials and their textures first: a mesh uploaded without them draws
    // untextured, which on a screen whose whole job is showing off an asset is
    // worse than showing nothing.
    for (const asset::Submesh& submesh : mesh->lods[0].submeshes) {
      const asset::Material* material = self->assets_->FindMaterial(submesh.material);
      if (!material)
        continue;
      for (asset::AssetId texture_id :
           {material->base_color, material->normal, material->metallic_roughness}) {
        if (!texture_id)
          continue;
        if (const asset::Texture* texture = self->assets_->FindTexture(texture_id))
          self->renderer_->UploadTexture(*texture);
      }
      self->renderer_->UploadMaterial(*material);
    }
    if (!self->renderer_->UploadMesh(*mesh))
      continue;

    self->load_model_mesh_ = mesh->id.hash;
    self->load_model_scale_ = screen.scale;
    for (int axis = 0; axis < 3; ++axis)
      self->load_model_rotation_[axis] = static_cast<f32>(screen.rotation[axis]);
    self->load_model_radius_ = mesh->bounds_radius > 0.01f ? mesh->bounds_radius : 1.0f;
    if (screen.description != 0) {
      if (const base::String* blurb = self->strings_.Find(screen.description))
        self->load_model_text_ = *blurb;
    }
    RX_INFO("load screen art: {} (scale {:.2f}, radius {:.2f} m){}", path, screen.scale,
            self->load_model_radius_,
            self->load_model_text_.empty() ? "" : " with a description");
    return;
  }
  RX_INFO("load screen art: {} screens, none of their meshes loaded", count);
}

void AppendLoadScreenModel(Engine& engine, render::FrameView& view) {
  Engine* const self = &engine;
  if (self->load_model_mesh_ == 0)
    return;

  // The record's own framing, plus a slow turn so it reads as an object in a
  // room rather than a still. Bethesda lets the player spin these by hand; this
  // just keeps it moving.
  const f32 spin = static_cast<f32>(NowSeconds() - self->load_started_) * 0.35f;
  const Vec3 at{0.0f, kLoadStageY, 0.0f};
  const f32 rx_deg = self->load_model_rotation_[0] * 0.01745329f;
  const f32 rz_deg = self->load_model_rotation_[2] * 0.01745329f;

  // The record's rotation, then the turn, composed as a quaternion because that
  // is what MakeTransform takes.
  const Quat pose = QuatFromAxisAngle({0, 1, 0}, spin + self->load_model_rotation_[1] * 0.01745329f) *
                    QuatFromAxisAngle({1, 0, 0}, rx_deg) * QuatFromAxisAngle({0, 0, 1}, rz_deg);
  const Mat4 model = MakeTransform(at, Normalize(pose), self->load_model_scale_);
  view.draws.push_back({self->load_model_mesh_, model, model});

  // Framed off the mesh's own bounds so a shield and a dragon wall both fill
  // the same amount of screen.
  const f32 reach = self->load_model_radius_ * self->load_model_scale_;
  const f32 distance = base::Max(reach * 2.6f, 0.6f);
  // Aimed BELOW the model so it sits in the upper half of the frame, clear of
  // the text group the mock anchors low. Looking straight at it centred the
  // model on the words.
  view.camera.eye = {distance * 0.5f, kLoadStageY + reach * 0.55f, distance};
  view.camera.target = {0.0f, kLoadStageY - reach * 0.75f, 0.0f};

  // Lit by the interior directional fill, not point lights.
  //
  // Point lights were the obvious choice and the wrong one twice over: their
  // radius has to cover the model, and these models range from a helmet to a
  // dragon wall, so a radius derived from bounds either fell short (an unlit
  // model) or was cranked up until it flooded the whole frame with grey. A
  // directional light has no falloff and no position: it lights the model the
  // same whatever its size, and lights nothing else because nothing else is
  // here.
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
  // A black stage for the model to stand on. The screen paints no background of
  // its own (that would bury the model), so the darkness has to come from the
  // renderer: `interior` is the existing flag for "suppress sky and
  // atmosphere", which is exactly what a loading screen wants. Restored on
  // close so the world gets its sky back.
  if (self->renderer_) {
    render::RenderSettings& s = self->renderer_->settings();
    self->load_prev_settings_ = s;
    // `interior` is the renderer's existing "no sky, no atmosphere" flag, which
    // is exactly a loading screen's backdrop. On its own it is not enough: the
    // interior ambient and fog left over from wherever the player last was
    // filled the frame with grey and the model read as a silhouette against it.
    // So dress the stage properly -- near-black ambient, no fill, no fog -- and
    // let the two lights in AppendLoadScreenModel be the only things lighting
    // anything.
    s.interior = true;
    s.interior_ambient = {0.015f, 0.015f, 0.018f};
    s.interior_directional_intensity = 0.0f;
    s.interior_fog_near_color = {0.0f, 0.0f, 0.0f};
    s.interior_fog_far_color = {0.0f, 0.0f, 0.0f};
    s.interior_fog_max = 0.0f;
    // Fixed exposure, and this is the one that matters. Auto exposure meters
    // the frame, and a loading screen is one lit object on black -- almost all
    // of it dark, so the metering opens right up, lifts the black stage to a
    // flat grey and flattens the model into a silhouette against it. Pinning
    // exposure is what makes it read as an object in a dark room.
    // Fixed, and LOW. This is what actually made the stage black. Auto
    // exposure meters a frame that is almost entirely empty and opens right up,
    // lifting the void to a flat grey with the model a silhouette on it; and
    // exposure 1.0 with auto off did the same. Pinning it low, and lighting the
    // model hard enough to answer, is what gives an object in a dark room.
    s.auto_exposure = false;
    s.exposure = LoadExposure.get();
    // And the actual background: `interior` alone still left the procedural
    // atmosphere painting the frame grey behind the model. These are the
    // switches that stop it being drawn at all.
    s.sky = false;
    s.clouds = false;
    // Nothing lights this stage but the two lamps in AppendLoadScreenModel.
    s.ambient = 0.0f;
    s.sun_intensity = 0.0f;
    s.interior_ambient = {0.018f, 0.018f, 0.018f};  // neutral, not the blue cast the sky IBL leaves
    s.interior_directional_color = {1.0f, 0.97f, 0.92f};
    s.interior_directional_intensity = LoadLight.get();
    // Travelling down and back over the viewer's shoulder: a three-quarter key.
    s.interior_directional_dir = {-0.45f, -0.55f, -0.70f};
  }
  // No HUD over a loading screen: the compass, vitals and gold counter belong
  // to a world the player is not in yet.
  self->game_ui_.SetHudVisible(false);
  self->game_ui_.OpenLoading(title);
  // Two frames, not one: the first is the one that replaces the menu, and with
  // double buffering the second is what guarantees the player has actually seen
  // it before the thread disappears into the load.
  ReportLoadPhase(engine, LoadPhase::kArchives, "Opening the game's archives");
  PresentLoadingFrame(engine);
}

// Fill the screen's view from a phase report. Does NOT present: the caller
// decides, because the two callers differ. A phase report during the load has
// to present (nothing else is drawing), while the per-frame hold afterwards
// must not (the host loop is drawing again, and a second submit per frame would
// just be waste).
void PushLoadingView(Engine& engine,
                     LoadPhase phase,
                     const base::String& detail,
                     const base::String& note,
                     f32 within) {
  Engine* const self = &engine;
  const int index =
      base::Clamp(static_cast<int>(phase), 0, static_cast<int>(LoadPhase::kCount) - 1);
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
}

void ReportLoadPhase(Engine& engine,
                     LoadPhase phase,
                     const base::String& detail,
                     const base::String& note,
                     f32 within) {
  Engine* const self = &engine;
  if (!self->load_screen_up_)
    return;

  PushLoadingView(engine, phase, detail, note, within);
  PresentLoadingFrame(engine);

  const int index =
      base::Clamp(static_cast<int>(phase), 0, static_cast<int>(LoadPhase::kCount) - 1);
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

void HoldLoadingUntilStreamed(Engine& engine) {
  Engine* const self = &engine;
  if (!self->load_screen_up_)
    return;
  self->load_wait_stream_ = true;
  RX_INFO("loaded {} in {:.1f}s; holding for the world to stream in",
          self->load_title_, NowSeconds() - self->load_started_);
}

void TickLoadingScreen(Engine& engine, f32 dt) {
  (void)dt;
  Engine* const self = &engine;
  if (!self->load_screen_up_ || !self->load_wait_stream_)
    return;

  // The world is not ready when LoadGameData returns.
  //
  // That call builds the streamer and asks for the first cells; it does not
  // wait for them. Taking the screen down at that point drops the player into a
  // world that then assembles itself in front of them for several seconds --
  // terrain, buildings and NPCs popping in around a camera that is already
  // live. So the card stays up until the streamer says it has caught up.
  //
  // This runs from the host loop rather than blocking like the phases above,
  // and it has to: cells are streamed BY that loop, so a blocking wait here
  // would hold the screen forever on a world that could never finish.
  const f64 elapsed = NowSeconds() - self->load_started_;
  const f64 minimum = base::Max(0.0f, LoadMinimumSeconds.get());
  const bool streamed = self->streamer_ && self->streamer_->caught_up();
  const bool timed_out = elapsed >= LoadMaxHoldSeconds.get();

  if ((streamed && elapsed >= minimum) || timed_out) {
    RX_INFO("world streamed in after {:.1f}s{}", elapsed, timed_out ? " [timeout]" : "");
    EndLoadingScreen(engine);
    return;
  }

  // Keep the screen honest while it waits: the last rail row stays lit and the
  // readout counts the cells as they land, so a long stream-in reads as work
  // happening rather than a hang.
  const size_t cells = self->streamer_ ? self->streamer_->loaded_cell_count() : 0;
  // 0, not 1: the streamer reports caught_up() and a resident count but no
  // target to divide by, so this stretch has no honest fraction. The bar parks
  // at the start of the last span (92%) and the ticking cell count carries it.
  // A bar that reached 100% and then sat for eight seconds reads as stuck.
  PushLoadingView(engine, LoadPhase::kWorld, "Streaming the world around you",
                  base::ToString(static_cast<u64>(cells)) + " cells resident", 0.0f);
}

void EndLoadingScreen(Engine& engine) {
  Engine* const self = &engine;
  if (!self->load_screen_up_)
    return;
  self->load_screen_up_ = false;
  self->load_wait_stream_ = false;
  if (self->renderer_)
    self->renderer_->settings() = self->load_prev_settings_;
  self->game_ui_.SetHudVisible(true);
  self->game_ui_.CloseLoading();
}

}  // namespace rx
