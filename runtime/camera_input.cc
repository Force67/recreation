#include "engine.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include <base/option.h>

#include "core/log.h"
#include "core/math.h"
#include "item_bridge.h"
#include "player_controller.h"
#include "world/components.h"

// Camera and player input: routes per-frame input to the right consumer (pause
// menu, editor, dialogue/container/journal modals, walk mode, or the free-fly
// camera), the walk-mode character controller, the scripted camera drivers
// (orbit / replay / cinematic showcase / record), and the debug physics toss.
namespace rx {

// Camera / capture overrides, populated from the environment by
// base::InitOptionsFromEnv() at startup.
static base::Option<const char*> Cam{"cam", nullptr, "RX_CAM"};
static base::Option<bool> Orbit{"orbit", false, "RX_ORBIT"};
static base::Option<bool> Editor{"editor", false, "RX_EDITOR"};
static base::Option<const char*> Record{"record", nullptr, "RX_RECORD"};
static base::Option<const char*> Replay{"replay", nullptr, "RX_REPLAY"};
static base::Option<bool> Showcase{"showcase", false, "RX_SHOWCASE"};
static base::Option<const char*> ShowcaseShots{"showcase.shots", nullptr, "RX_SHOWCASE_SHOTS"};
static base::Option<bool> ShowcaseQuit{"showcase.quit", false, "RX_SHOWCASE_QUIT"};
// RX_TRAILER layers the cinematic trailer (cycling weather + render mode, with
// title cards) over the showcase flythrough; it implies RX_SHOWCASE.
static base::Option<bool> Trailer{"trailer", false, "RX_TRAILER"};
static base::Option<const char*> TrailerTitle{"trailer.title", "RECREATION", "RX_TRAILER_TITLE"};
// Safety cap on the per-game loading hold: if a map never reports caught-up
// (huge worldspace, missing assets), reveal it anyway after this many seconds.
// Generous, because the wide preload diorama (radius 4) is a lot of cells.
static constexpr f32 kTrailerMaxLoadHold = 25.0f;
static base::Option<bool> AutoAttack{"auto.attack", false, "RX_AUTO_ATTACK"};

void Engine::UpdateCamera(f32 frame_delta) {
  if (!window_) return;
  // The first-run setup wizard owns all input while it is up, ahead of the menu
  // it hands off to once the player finishes (which clears first_run_active_).
  if (first_run_active_ && game_ui_.first_run_open()) {
    UpdateFirstRun(frame_delta);
    return;
  }
  // The NEXUS main menu owns all input while it is up; gameplay/camera stays
  // frozen until a universe is entered (which clears main_menu_active_).
  if (main_menu_active_ && game_ui_.main_menu_open()) {
    UpdateMainMenu(frame_delta);
    return;
  }
  // Character creation owns all input while it is up: it hit-tests its own
  // overlay against a free (absolute) cursor and orbits the framed camera on a
  // viewport drag, so the fly camera and gameplay toggles stay frozen.
  if (chargen_ && chargen_->active()) {
    window_->SetRelativeMouseMode(false);
    chargen_->Update(window_->input(), frame_delta);
    return;
  }

  const InputState& input = window_->input();

  // The pause menu freezes the camera and frees the cursor so it can click.
  bool menu = game_ui_.menu_open();
  bool kb = debug_ui_.wants_keyboard();
  // Modal overlays that consume Esc; captured before the branches below so the
  // Esc that closes one does not also open the pause menu this frame.
  bool modal = interaction_->dialogue_open() || interaction_->container_open();
  // The map editor takes over navigation and input while on; its own Esc cancels
  // (clear brush / cancel move), so the pause menu yields to it.
  const bool editor_on = editor_ && editor_->active();

  if (actions_->pressed(Action::kToggleWalk) && !menu && !kb && !modal && !editor_on &&
      actors_->HasPlayer()) {
    ctx_.walk_mode = !ctx_.walk_mode;
    RX_INFO("walk mode {}",
             ctx_.walk_mode ? "on (WASD move, Shift run, Space jump, C view)" : "off");
  }
  if (actions_->pressed(Action::kToggleThirdPerson) && !menu && !kb && !modal && !editor_on)
    ctx_.third_person = !ctx_.third_person;
  if (actions_->pressed(Action::kToggleJournal) && !menu && !kb && !modal && !editor_on)
    quest_->ToggleJournal();
  // The war map (M): the Civil War campaign board, a modern overlay over the
  // managed campaign state.
  if (input.key_pressed(Key::kM) && !menu && !kb && !modal && !editor_on)
    war_map_open_ = !war_map_open_;

  if (editor_on) {
    // Free-fly builder camera: right mouse looks, WASD/QE move (unless the search
    // box has the keyboard). The editor handles picking, placing and editing.
    const bool allow = !menu && !kb;
    const bool typing = editor_->wants_keyboard();
    camera_.Update(input, *actions_, allow, allow && !typing, frame_delta);
    window_->SetRelativeMouseMode(!menu && camera_.looking());
    // The map editor is a keyboard dev tool (modifier combos: Ctrl+Z, Shift+R),
    // so it keeps reading raw keys rather than going through the action layer.
    editor_->Update(input, frame_delta, allow);
  } else if (interaction_->container_open()) {
    interaction_->UpdateContainerInput(input, *actions_);  // Esc / pad B closes the loot view
    interaction_->UpdateInteraction(false);  // freeze movement/activation while looting
  } else if (interaction_->dialogue_open()) {
    // dpad/arrows highlight, A/Enter or 1-4 select, B/Esc leaves.
    interaction_->UpdateDialogueInput(input, *actions_);
    interaction_->UpdateInteraction(false);  // freeze movement/activation while talking
  } else if (quest_->journal_open()) {
    // The journal is a modal overlay: a number key pins that quest to track,
    // pad B closes it; movement is frozen while it is open.
    const Key num[4] = {Key::k1, Key::k2, Key::k3, Key::k4};
    for (int i = 0; i < 4; ++i)
      if (input.key_pressed(num[i])) quest_->PinJournalSlot(i);
    if (actions_->pressed(Action::kMenuCancel)) quest_->ToggleJournal();
    interaction_->UpdateInteraction(false);
  } else if (carriage_ && carriage_->riding()) {
    // Seated as a carriage passenger: locomotion stands down, the carriage pins
    // the player to the seat and frames the camera; activate again to dismount.
    carriage_->UpdateRide(frame_delta);
    interaction_->UpdateInteraction(actions_->pressed(Action::kActivate) && !menu && !kb);
  } else if (ctx_.walk_mode && actors_->HasPlayer()) {
    WalkUpdate(frame_delta, !menu && !kb);
    interaction_->UpdateInteraction(actions_->pressed(Action::kActivate) && !menu && !kb);
    // Drop the most recent inventory item into the world (walk mode only).
    if (ctx_.items && actions_->pressed(Action::kDropItem) && !menu && !kb) ctx_.items->DropLast();
  } else {
    bool allow_mouse = !menu && (!debug_ui_.wants_mouse() || camera_.looking());
    bool allow_keyboard = !menu && !kb;
    camera_.Update(input, *actions_, allow_mouse, allow_keyboard, frame_delta);
    window_->SetRelativeMouseMode(!menu && camera_.looking());
    interaction_->UpdateInteraction(false);  // clears any stale prompt outside walk mode
  }

  interaction_->SyncHud();   // mirror the conversation / loot view into the HUD
  DriveCamera(frame_delta);  // orbit / replay overrides + record

  if (actions_->pressed(Action::kToggleDebug) && !kb) debug_ui_.ToggleVisible();
  if (actions_->pressed(Action::kToggleTrace) && !kb) debug_ui_.ToggleTrace();
  if (actions_->pressed(Action::kToggleQuests) && !kb) debug_ui_.ToggleQuests();
  if (actions_->pressed(Action::kToggleEditor) && !kb && editor_) editor_->Toggle();
  if (actions_->pressed(Action::kThrowDebug) && !menu && !kb && !ctx_.walk_mode && !editor_on)
    ThrowPhysicsCube();
  // DualSense adaptive-trigger demo: readying a weapon toggles right-trigger
  // resistance (a no-op on Xbox / when disabled in settings).
  if (actions_->pressed(Action::kReady) && !menu && !kb && !editor_on && window_ &&
      input_map_->adaptive_triggers) {
    weapon_trigger_ = !weapon_trigger_;
    TriggerEffect fx;
    if (weapon_trigger_) {
      fx.type = TriggerEffect::Type::kWeapon;
      fx.start = 70;
      fx.strength = 160;
    }
    window_->SetTriggerEffect(false, true, fx);
  }
  // The editor owns Esc (cancel brush / move); only open the pause menu outside it.
  if (actions_->pressed(Action::kToggleMenu) && !kb && !modal && !editor_on) game_ui_.ToggleMenu();
  if (game_ui_.quit_requested()) RequestQuit();
}

void Engine::LookCameraAt(const Vec3& eye, const Vec3& center) {
  camera_.set_position(eye);
  Vec3 d = Normalize(center - eye);
  camera_.set_yaw_pitch(std::atan2(d.x, -d.z),
                        std::asin(std::clamp(d.y, -1.0f, 1.0f)));  // forward() convention
}

void Engine::DriveCamera(f32 dt) {
  if (!cam_init_) {
    cam_init_ = true;
    // RX_CAM="x,y,z,yaw,pitch" pins the camera for a framed capture (handy for
    // screenshots that must show a specific vantage, e.g. two worlds side by side).
    if (const char* c = Cam.get()) {
      Vec3 p{};
      f32 yaw = 0, pitch = 0;
      if (std::sscanf(c, "%f,%f,%f,%f,%f", &p.x, &p.y, &p.z, &yaw, &pitch) >= 3) {
        camera_.set_position(p);
        camera_.set_yaw_pitch(yaw, pitch);
      }
    }
    cam_orbit_ = bool(Orbit);
    // RX_EDITOR boots straight into the map editor (the catalog is ready once
    // the records are loaded), so a capture or a builder session can skip F4.
    if (Editor && editor_ && !editor_->active()) editor_->Toggle();
    if (const char* r = Record.get()) cam_record_ = std::fopen(r, "wb");
    if (const char* p = Replay.get()) {
      if (std::FILE* f = std::fopen(p, "rb")) {
        f32 rec[7];
        while (std::fread(rec, sizeof(f32), 7, f) == 7) {
          cam_replay_.push_back({rec[0], {rec[1], rec[2], rec[3]}, {rec[4], rec[5], rec[6]}});
        }
        std::fclose(f);
        RX_INFO("camera replay: {} keys from {}", cam_replay_.size(), p);
      }
    }
    // RX_SHOWCASE flies a smooth cinematic pass over every loaded worldspace in
    // one take. RX_SHOWCASE_SHOTS=<dir> writes a regression PNG at each marked
    // beat; RX_SHOWCASE_QUIT exits when the pass ends (headless benchmark).
    if (Showcase || Trailer) {
      // Multi-game trailer: collapse the side-by-side regions onto one shared
      // center and stream the games one at a time. Must run before BuildShowcase
      // so the camera path is built over the collapsed regions.
      if (Trailer) SetupTrailerStreaming();
      BuildShowcase();
      cam_showcase_ = !showcase_.empty();
      if (const char* d = ShowcaseShots.get()) showcase_shot_dir_ = d;
      showcase_quit_ = bool(ShowcaseQuit);
      if (cam_showcase_) {
        ctx_.walk_mode = false;         // the cinematic owns the camera
        game_ui_.SetHudVisible(false);  // clean frames: no crosshair / compass / bars
        RX_INFO("showcase: {} waypoints over {} region(s), {:.1f}s{}", showcase_.size(),
                 showcase_regions_.size(), showcase_.duration(),
                 showcase_shot_dir_.empty() ? "" : " (capturing)");
        // The trailer rides the same path, adding the weather/render-mode cycle
        // and the title cards. It owns the weather and the render settings while
        // it runs, so the debug panels step aside for clean frames.
        if (Trailer) {
          BuildTrailer();
          cam_trailer_ = !trailer_.empty();
          if (cam_trailer_) {
            debug_ui_.SetAllVisible(false);
            debug_ui_.SetTrailerOverlay(&current_trailer_overlay_);
            RX_INFO("trailer: {} title card(s) over {:.1f}s", showcase_regions_.size(),
                     trailer_.duration());
          }
        }
      } else {
        RX_WARN("RX_SHOWCASE/RX_TRAILER set but no worldspaces to fly over");
      }
    }
  }
  // Freeze the trailer clock while a freshly cut-to game streams in, so the camera
  // never glides over a half-loaded map; trailer_load_elapsed_ caps the wait.
  if (cam_trailer_ && trailer_loading_)
    trailer_load_elapsed_ += dt;
  else
    cam_time_ += dt;

  if (cam_showcase_) {
    f32 prev = cam_time_ - dt;
    ShowcasePose p = showcase_.Sample(cam_time_);
    LookCameraAt(p.eye, p.target);
    // Trailer: drive the weather, render mode and on-screen chrome off the same
    // clock. Weather is pushed through the live override the frame loop already
    // applies; the render mode is only re-applied when it actually changes so the
    // path tracer is not needlessly reset.
    if (cam_trailer_) {
      // Multi-game: at a beat boundary, cut to the next game (unloading the prior)
      // and hold on a loading screen until it has streamed in.
      if (trailer_sequential_) {
        int want = trailer_.ActiveBeatIndex(cam_time_);
        if (want != trailer_active_domain_) {
          SwitchTrailerDomain(want);
          trailer_loading_ = true;
          trailer_load_elapsed_ = 0.0f;
        }
        if (trailer_loading_ &&
            (TrailerActiveLoaded() || trailer_load_elapsed_ >= kTrailerMaxLoadHold)) {
          trailer_loading_ = false;
          RX_INFO("trailer: {} ready ({:.1f}s){}",
                   showcase_regions_[trailer_active_domain_].name, trailer_load_elapsed_,
                   TrailerActiveLoaded() ? "" : " [timeout]");
        }
      }
      TrailerState ts = trailer_.At(cam_time_);
      director_.SetOverride(&ts.weather);  // the trailer owns the weather
      if (!trailer_mode_applied_ || ts.mode != applied_trailer_mode_) {
        ApplyTrailerRenderMode(ts.mode);
        applied_trailer_mode_ = ts.mode;
        trailer_mode_applied_ = true;
      }
      // While holding, black out the scene and show the loading card for the game
      // that is streaming in.
      if (trailer_loading_) {
        ts.overlay.loading_label = ts.overlay.title;  // incoming game (may be empty)
        ts.overlay.fade = 1.0f;
        ts.overlay.title_alpha = 0.0f;
        ts.overlay.intro_alpha = 0.0f;
        ts.overlay.badge_alpha = 0.0f;
        ts.overlay.loading = true;
      }
      current_trailer_overlay_ = std::move(ts.overlay);
    }
    if (!showcase_shot_dir_.empty()) {
      std::string label;
      int idx = showcase_.CaptureCrossed(prev, cam_time_, &label);
      if (idx >= 0) {
        for (char& ch : label) {
          bool ok =
              (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
          if (!ok) ch = '_';
        }
        char path[1024];
        std::snprintf(path, sizeof(path), "%s/%02d_%s.png", showcase_shot_dir_.c_str(), idx,
                      label.c_str());
        renderer_->CaptureScreenshot(path);
        RX_INFO("showcase capture: {}", path);
      }
    }
    if (!showcase_done_) {
      // Benchmark steady-state render: skip the first second of warmup and any
      // half-second-plus frame (cold streaming/IO hitches, not GPU time). Real
      // mid-flight stutter (down to ~2 fps) is still counted, so it shows up.
      if (dt > 0 && dt < 0.5f && cam_time_ >= 1.0f && !trailer_loading_) {
        showcase_dt_min_ = std::min(showcase_dt_min_, dt);
        showcase_dt_max_ = std::max(showcase_dt_max_, dt);
        showcase_bench_time_ += dt;
        ++showcase_frames_;
      }
      if (cam_time_ >= showcase_.duration()) {
        showcase_done_ = true;
        f32 avg =
            showcase_frames_ > 0 ? showcase_bench_time_ / static_cast<f32>(showcase_frames_) : 0.0f;
        RX_INFO("showcase done: {} frames over {:.1f}s, avg {:.0f} fps (min {:.0f}, max {:.0f})",
                 showcase_frames_, showcase_bench_time_, avg > 0 ? 1.0f / avg : 0.0f,
                 showcase_dt_max_ > 0 ? 1.0f / showcase_dt_max_ : 0.0f,
                 showcase_dt_min_ > 0 ? 1.0f / showcase_dt_min_ : 0.0f);
        if (showcase_quit_) RequestQuit();
      }
    }
  } else if (cam_orbit_) {
    f32 a = cam_time_ * 0.4f;  // radians/sec
    Vec3 center{0.0f, 1.0f, 0.0f};
    LookCameraAt({center.x + std::cos(a) * 6.0f, 2.4f, center.z + std::sin(a) * 6.0f}, center);
  } else if (!cam_replay_.empty()) {
    // Linear interpolation between the bracketing keys for the current time.
    const CamKey* lo = &cam_replay_[0];
    const CamKey* hi = lo;
    for (const CamKey& k : cam_replay_) {
      if (k.t <= cam_time_) lo = &k;
      if (k.t >= cam_time_) {
        hi = &k;
        break;
      }
    }
    f32 span = hi->t - lo->t;
    f32 u = span > 1e-5f ? std::clamp((cam_time_ - lo->t) / span, 0.0f, 1.0f) : 0.0f;
    auto mix = [&](const Vec3& a, const Vec3& b) { return a + (b - a) * u; };
    LookCameraAt(mix(lo->pos, hi->pos), mix(lo->target, hi->target));
  }

  if (cam_record_) {
    Vec3 p = camera_.position(), t = camera_.target();
    f32 rec[7] = {cam_time_, p.x, p.y, p.z, t.x, t.y, t.z};
    std::fwrite(rec, sizeof(f32), 7, cam_record_);
    std::fflush(cam_record_);  // survive a timeout kill
  }
}

void Engine::BuildShowcase() {
  // Per-worldspace drone beats, as offsets in metres from the region centre C
  // (ground level). Regions are placed along +X (east), so each pass enters from
  // the west and exits east, which keeps the glide between worldspaces short.
  auto wp = [](Vec3 eye, Vec3 look, f32 travel, bool capture, std::string label) {
    return ShowcaseCamera::Waypoint{eye, look, travel, capture, std::move(label)};
  };
  // Lead-in: hold on the opening vantage so the primary world (which streams
  // around the camera) has time to populate before the move and the first
  // capture. Secondary worldspaces stream around their own fixed anchor, so they
  // are already loaded by the time the camera reaches them.
  if (!showcase_regions_.empty()) {
    const Vec3 c0 = showcase_regions_[0].center;
    showcase_.Add(wp(c0 + Vec3{-180, 110, 80}, c0 + Vec3{-20, 25, 0}, 0.0f, false, {}));
  }
  // Every waypoint sits at ~zero velocity (smoothstep), so each is a clean,
  // well-framed still: capture them all for broad regression coverage and so a
  // good YouTube frame can be picked from the set. Files sort by play order.
  for (size_t k = 0; k < showcase_regions_.size(); ++k) {
    const Vec3 c = showcase_regions_[k].center;
    const std::string& n = showcase_regions_[k].name;
    // Flythrough time this region's block starts (the camera leaving the prior
    // region's last beat), so the trailer can fade its location title in here.
    showcase_region_start_.push_back(showcase_.duration());
    if (k > 0) {
      // High traveling glide over the seam: both worldspaces are in frame here,
      // which makes it both a strong showcase beat and a regression mark.
      Vec3 mid = (showcase_regions_[k - 1].center + c) * 0.5f;
      showcase_.Add(wp(mid + Vec3{0, 170, 70}, mid + Vec3{70, 0, -20}, 9.0f, true, "seam"));
    }
    // Establishing: high to the west, looking down into the region. For region 0
    // this is the same pose as the lead-in, so the camera holds there first.
    showcase_.Add(
        wp(c + Vec3{-180, 110, 80}, c + Vec3{-20, 25, 0}, k == 0 ? 5.0f : 7.0f, true, n + "_wide"));
    // Descending push-in toward the centre.
    showcase_.Add(wp(c + Vec3{-70, 38, -30}, c + Vec3{40, 15, -60}, 5.0f, true, n + "_descend"));
    // Low skim across the centre.
    showcase_.Add(wp(c + Vec3{-10, 22, 25}, c + Vec3{110, 10, -10}, 7.0f, true, n + "_skim"));
    // Crane up and east, revealing the vista and setting up the exit.
    showcase_.Add(wp(c + Vec3{120, 92, 70}, c + Vec3{30, 15, 0}, 5.0f, true, n + "_reveal"));
  }
}

namespace {
// A clean trailer caption for a loaded game: a short title plus a tagline,
// derived from the content domain's profile name (uppercased as a fallback).
struct TrailerCaption {
  std::string title;
  std::string subtitle;
};
std::string Upper(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}
TrailerCaption CaptionFor(const std::string& name) {
  if (name.rfind("Skyrim", 0) == 0) return {"SKYRIM", "THE ELDER SCROLLS V"};
  if (name.rfind("Fallout 4", 0) == 0) return {"FALLOUT 4", "THE COMMONWEALTH"};
  if (name.rfind("Fallout 76", 0) == 0) return {"FALLOUT 76", "APPALACHIA"};
  if (name.rfind("Starfield", 0) == 0) return {"STARFIELD", "THE SETTLED SYSTEMS"};
  return {Upper(name), ""};
}
}  // namespace

void Engine::BuildTrailer() {
  trailer_ = TrailerDirector{};  // fresh, in case of a rebuild
  if (showcase_regions_.empty()) return;
  if (const char* title = TrailerTitle.get())
    trailer_.set_intro(title, "A BETHESDA ENGINE, REIMAGINED");
  const f32 total = showcase_.duration();
  for (size_t k = 0; k < showcase_regions_.size(); ++k) {
    const f32 start = k < showcase_region_start_.size() ? showcase_region_start_[k] : 0.0f;
    const f32 end = (k + 1 < showcase_region_start_.size()) ? showcase_region_start_[k + 1] : total;
    TrailerCaption cap = CaptionFor(showcase_regions_[k].name);
    trailer_.AddBeat({start, end, std::move(cap.title), std::move(cap.subtitle)});
  }
  trailer_.set_duration(total);
}

void Engine::ApplyTrailerRenderMode(TrailerRenderMode mode) {
  render::RenderSettings& s = renderer_->settings();
  switch (mode) {
    case TrailerRenderMode::kRaster:
      // Pure raster fallbacks: cascaded shadows, screen-space AO/reflections/GI.
      s.path_trace = false;
      s.rt_shadows = false;
      s.shadow_maps = true;
      s.rtao = false;
      s.ssao = true;
      s.rt_reflections = false;
      s.water_reflections = false;
      s.ssr = true;
      s.ddgi = false;
      s.rcgi = false;  // no ray query in the raster fallback
      s.ssgi = true;
      break;
    case TrailerRenderMode::kRayTracing:
      // Full ray-traced hybrid: rt shadows, AO, reflections and RCGI (idTech8-style
      // radiance-cached GI, the AC-Shadows adoption). ddgi stays enabled as the
      // automatic fallback: the renderer's rcgi_active predicate runs whichever
      // is available (rcgi when the system is up, ddgi otherwise).
      s.path_trace = false;
      s.rt_shadows = true;
      s.shadow_maps = false;
      s.rtao = true;
      s.ssao = false;
      s.rt_reflections = true;
      s.water_reflections = true;
      s.ssr = false;
      s.ddgi = true;
      s.rcgi = true;
      s.ssgi = false;
      break;
    case TrailerRenderMode::kPathTracing:
      // Reference progressive path tracer, over the rt feature set. (Noisy on a
      // moving camera and grass/distant LOD are excluded from rays on this branch;
      // the denoised, grass-aware PT lives on feat/pathtrace-denoise-vegetation.)
      s.rt_shadows = true;
      s.rtao = true;
      s.rt_reflections = true;
      s.water_reflections = true;
      s.ddgi = true;
      s.rcgi = true;  // inert while path_trace is on (rcgi_world gates it off), so
                      // toggling PT off in this mode falls back to RCGI, not flat GI
      s.path_trace = true;
      break;
  }
}

world::CellStreamer* Engine::TrailerStreamer(int region_index) {
  if (region_index < 0 || static_cast<size_t>(region_index) >= showcase_regions_.size())
    return nullptr;
  world::CellStreamer* s = showcase_regions_[region_index].streamer;
  return s ? s : streamer_.get();  // null region streamer means the primary game
}

void Engine::SetupTrailerStreaming() {
  // Only sequence when more than the primary game renders; a single map keeps the
  // simple in-place trailer (all regions already share one center).
  if (extra_streamers_.empty() || showcase_regions_.size() < 2) return;
  trailer_sequential_ = true;
  const Vec3 center = showcase_regions_[0].center;  // the primary start-cell center

  // Preload each region as a wide, fixed diorama anchored at the shared center
  // rather than streaming around the moving camera. This fixes three things:
  //  - the hero area is real near-cell geometry, which ray tracing / path tracing
  //    can see (distant LOD proxies are excluded from the TLAS, so a camera-chased
  //    half-loaded region showed holes in those modes);
  //  - nothing streams while the camera flies, so no mid-shot stutter or pop-in;
  //  - the region's ground is aligned to the camera's height, so the drone never
  //    clips below the terrain (secondary worlds sit at their own LAND height).
  world::CellStreamer::Settings preload;
  preload.load_radius = 3;     // ~120 m of resident near cells around the subject
  preload.mesh_budget = 48;    // load fast (hidden behind the black loading hold)
  preload.ref_budget = 768;
  preload.distant_lod = true;  // horizon fill (RX_DISTANT_LOD also forces this on)
  preload.distant_budget = 12;
  preload.grass_density = config_.grass_density;

  for (size_t k = 0; k < showcase_regions_.size(); ++k) {
    world::CellStreamer* s = TrailerStreamer(static_cast<int>(k));
    if (!s) continue;
    // The region center in this domain's own (pre-offset) coordinates: the primary
    // is the start cell under the shared center; a secondary uses its anchor.
    const Vec3 anchor = (k == 0) ? Vec3{center.x, 0.0f, center.z} : s->fixed_anchor();
    f32 ground = 0.0f;
    s->GroundHeight(anchor.x, anchor.z, &ground);  // own-coords ground (0 if none)
    s->set_fixed_anchor(anchor);
    s->set_world_offset({center.x - anchor.x, center.y - ground, center.z - anchor.z});
    s->Configure(preload);
    if (k != 0) s->UnloadAllCells(*world_);  // only the active game stays resident
  }
  // Collapse all regions onto the shared center: the drone repeats its hero move
  // in place for each game while the active domain switches under the fade-cut.
  for (auto& r : showcase_regions_) r.center = center;
  trailer_active_domain_ = 0;  // the primary streams first
  trailer_loading_ = true;     // hold on the loading screen until it has streamed in
  trailer_load_elapsed_ = 0.0f;
}

bool Engine::TrailerActiveLoaded() {
  world::CellStreamer* s = TrailerStreamer(trailer_active_domain_);
  return !s || s->caught_up();
}

void Engine::SwitchTrailerDomain(int region_index) {
  if (region_index == trailer_active_domain_) return;
  if (world::CellStreamer* prev = TrailerStreamer(trailer_active_domain_))
    prev->UnloadAllCells(*world_);  // drop the outgoing map; the incoming streams in
  trailer_active_domain_ = region_index;
  if (region_index >= 0 && static_cast<size_t>(region_index) < showcase_regions_.size())
    RX_INFO("trailer: cut to {} (loading)", showcase_regions_[region_index].name);
}

void Engine::WalkUpdate(f32 dt, bool allow) {
  const InputState& input = window_->input();
  window_->SetRelativeMouseMode(true);  // FPS-style mouse look in walk mode

  // Lazily assemble the rx character + camera rig onto the player actor entity
  // the first walk-mode frame after the player spawns. Skyrim-authentic
  // locomotion + FP/TP camera live in the controller; this function is now input
  // gathering (auto-walk, melee, battle cam) plus delegation.
  if (!player_controller_)
    player_controller_ = std::make_unique<PlayerController>(ctx_, *actors_, *input_map_);
  if (!player_controller_->assembled() && !player_controller_->Assemble()) return;

  // Auto-walk test hook: head for the active quest marker / guide mark when one is
  // set (routed through pathfinding so the player rounds interior walls), else
  // coast along the current camera facing.
  Vec3 auto_move{};
  const bool auto_walk = ctx_.auto_walk;
  if (auto_walk) {
    const Vec3 facing{std::sin(ctx_.cam_yaw), 0, -std::cos(ctx_.cam_yaw)};
    Vec3 ppos;
    if (ctx_.auto_walk_has_goal && actors_->PlayerWorldPos(&ppos)) {
      const Vec3 wp = npc_->PathToward(ppos, ctx_.auto_walk_goal);
      Vec3 to{wp.x - ppos.x, 0, wp.z - ppos.z};
      const f32 len = Length(to);
      auto_move = len > 0.5f ? to * (1.0f / len) : facing;
    } else {
      auto_move = facing;
    }
  }

  // Step locomotion + camera. Writes ctx_.walk_eye / walk_target / cam_yaw and
  // feeds the actor; `body` is the player feet for the melee / battle systems.
  Vec3 body{};
  player_controller_->Update(dt, input, *actions_, allow, auto_walk, auto_move, &body);

  // Melee: a left-click / right-trigger swing strikes the NPC the player faces,
  // so the player can fight (clear a fort, join a battle). Aim is the camera yaw.
  // RX_AUTO_ATTACK swings on a timer for headless playthrough verification.
  const bool auto_attack = bool(AutoAttack);
  bool swing = allow && actions_->pressed(Action::kAttack);
  if (auto_attack) {
    auto_attack_timer_ -= dt;
    if (auto_attack_timer_ <= 0.0f) {
      auto_attack_timer_ = 0.7f;
      swing = true;
    }
  }
  if (swing) npc_->PlayerMeleeStrike(body, ctx_.cam_yaw);

  // A staged field battle takes over the view with an elevated spectator framing,
  // so the clash is visible even when the player wedged against terrain.
  Vec3 beye, btarget;
  if (npc_->BattleCam(&beye, &btarget)) {
    ctx_.walk_eye = beye;
    ctx_.walk_target = btarget;
  }
}

void Engine::ThrowPhysicsCube() {
  if (!physics_->initialized() || !physics_cube_mesh_) return;
  Vec3 forward = camera_.forward();
  Vec3 origin = camera_.position() + forward * 0.8f;
  // Wood-ish density: heavy enough to splash, light enough to float.
  physics::BodyId body =
      physics_->AddDynamicBox(origin, {0.25f, 0.25f, 0.25f}, 350.0f, forward * 14.0f);
  if (!body) return;
  ecs::Entity entity = world_->Create();
  world_->Add(entity, world::Transform{.position = {origin.x, origin.y, origin.z}});
  world_->Add(entity, world::Renderable{physics_cube_mesh_});
  physics_entities_.push_back({body, entity});
  if (window_ && input_map_->rumble) window_->SetRumble(0.35f, 0.7f, 180);  // toss kick
}

}  // namespace rx
