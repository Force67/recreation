#include "engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "core/log.h"
#include "core/math.h"
#include "world/components.h"

// Camera and player input: routes per-frame input to the right consumer (pause
// menu, editor, dialogue/container/journal modals, walk mode, or the free-fly
// camera), the walk-mode character controller, the scripted camera drivers
// (orbit / replay / cinematic showcase / record), and the debug physics toss.
namespace rec {

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

  if (actions_.pressed(Action::kToggleWalk) && !menu && !kb && !modal && !editor_on &&
      actors_->HasPlayer()) {
    ctx_.walk_mode = !ctx_.walk_mode;
    REC_INFO("walk mode {}",
             ctx_.walk_mode ? "on (WASD move, Shift run, Space jump, C view)" : "off");
  }
  if (actions_.pressed(Action::kToggleThirdPerson) && !menu && !kb && !modal && !editor_on)
    ctx_.third_person = !ctx_.third_person;
  if (actions_.pressed(Action::kToggleJournal) && !menu && !kb && !modal && !editor_on)
    quest_->ToggleJournal();

  if (editor_on) {
    // Free-fly builder camera: right mouse looks, WASD/QE move (unless the search
    // box has the keyboard). The editor handles picking, placing and editing.
    const bool allow = !menu && !kb;
    const bool typing = editor_->wants_keyboard();
    camera_.Update(input, actions_, allow, allow && !typing, frame_delta);
    window_->SetRelativeMouseMode(!menu && camera_.looking());
    // The map editor is a keyboard dev tool (modifier combos: Ctrl+Z, Shift+R),
    // so it keeps reading raw keys rather than going through the action layer.
    editor_->Update(input, frame_delta, allow);
  } else if (interaction_->container_open()) {
    interaction_->UpdateContainerInput(input, actions_);  // Esc / pad B closes the loot view
    interaction_->UpdateInteraction(false);  // freeze movement/activation while looting
  } else if (interaction_->dialogue_open()) {
    // dpad/arrows highlight, A/Enter or 1-4 select, B/Esc leaves.
    interaction_->UpdateDialogueInput(input, actions_);
    interaction_->UpdateInteraction(false);  // freeze movement/activation while talking
  } else if (quest_->journal_open()) {
    // The journal is a modal overlay: a number key pins that quest to track,
    // pad B closes it; movement is frozen while it is open.
    const Key num[4] = {Key::k1, Key::k2, Key::k3, Key::k4};
    for (int i = 0; i < 4; ++i)
      if (input.key_pressed(num[i])) quest_->PinJournalSlot(i);
    if (actions_.pressed(Action::kMenuCancel)) quest_->ToggleJournal();
    interaction_->UpdateInteraction(false);
  } else if (ctx_.walk_mode && actors_->HasPlayer()) {
    WalkUpdate(frame_delta, !menu && !kb);
    interaction_->UpdateInteraction(actions_.pressed(Action::kActivate) && !menu && !kb);
  } else {
    bool allow_mouse = !menu && (!debug_ui_.wants_mouse() || camera_.looking());
    bool allow_keyboard = !menu && !kb;
    camera_.Update(input, actions_, allow_mouse, allow_keyboard, frame_delta);
    window_->SetRelativeMouseMode(!menu && camera_.looking());
    interaction_->UpdateInteraction(false);  // clears any stale prompt outside walk mode
  }

  interaction_->SyncHud();   // mirror the conversation / loot view into the HUD
  DriveCamera(frame_delta);  // orbit / replay overrides + record

  if (actions_.pressed(Action::kToggleDebug) && !kb) debug_ui_.ToggleVisible();
  if (actions_.pressed(Action::kToggleTrace) && !kb) debug_ui_.ToggleTrace();
  if (actions_.pressed(Action::kToggleQuests) && !kb) debug_ui_.ToggleQuests();
  if (actions_.pressed(Action::kToggleEditor) && !kb && editor_) editor_->Toggle();
  if (actions_.pressed(Action::kThrowDebug) && !menu && !kb && !ctx_.walk_mode && !editor_on)
    ThrowPhysicsCube();
  // DualSense adaptive-trigger demo: readying a weapon toggles right-trigger
  // resistance (a no-op on Xbox / when disabled in settings).
  if (actions_.pressed(Action::kReady) && !menu && !kb && !editor_on && window_ &&
      input_map_.adaptive_triggers) {
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
  if (actions_.pressed(Action::kToggleMenu) && !kb && !modal && !editor_on) game_ui_.ToggleMenu();
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
    // REC_CAM="x,y,z,yaw,pitch" pins the camera for a framed capture (handy for
    // screenshots that must show a specific vantage, e.g. two worlds side by side).
    if (const char* c = std::getenv("REC_CAM")) {
      Vec3 p{};
      f32 yaw = 0, pitch = 0;
      if (std::sscanf(c, "%f,%f,%f,%f,%f", &p.x, &p.y, &p.z, &yaw, &pitch) >= 3) {
        camera_.set_position(p);
        camera_.set_yaw_pitch(yaw, pitch);
      }
    }
    cam_orbit_ = std::getenv("REC_ORBIT") != nullptr;
    // REC_EDITOR boots straight into the map editor (the catalog is ready once
    // the records are loaded), so a capture or a builder session can skip F4.
    if (std::getenv("REC_EDITOR") && editor_ && !editor_->active()) editor_->Toggle();
    if (const char* r = std::getenv("REC_RECORD")) cam_record_ = std::fopen(r, "wb");
    if (const char* p = std::getenv("REC_REPLAY")) {
      if (std::FILE* f = std::fopen(p, "rb")) {
        f32 rec[7];
        while (std::fread(rec, sizeof(f32), 7, f) == 7) {
          cam_replay_.push_back({rec[0], {rec[1], rec[2], rec[3]}, {rec[4], rec[5], rec[6]}});
        }
        std::fclose(f);
        REC_INFO("camera replay: {} keys from {}", cam_replay_.size(), p);
      }
    }
    // REC_SHOWCASE flies a smooth cinematic pass over every loaded worldspace in
    // one take. REC_SHOWCASE_SHOTS=<dir> writes a regression PNG at each marked
    // beat; REC_SHOWCASE_QUIT exits when the pass ends (headless benchmark).
    if (std::getenv("REC_SHOWCASE")) {
      BuildShowcase();
      cam_showcase_ = !showcase_.empty();
      if (const char* d = std::getenv("REC_SHOWCASE_SHOTS")) showcase_shot_dir_ = d;
      showcase_quit_ = std::getenv("REC_SHOWCASE_QUIT") != nullptr;
      if (cam_showcase_) {
        ctx_.walk_mode = false;         // the cinematic owns the camera
        game_ui_.SetHudVisible(false);  // clean frames: no crosshair / compass / bars
        REC_INFO("showcase: {} waypoints over {} region(s), {:.1f}s{}", showcase_.size(),
                 showcase_regions_.size(), showcase_.duration(),
                 showcase_shot_dir_.empty() ? "" : " (capturing)");
      } else {
        REC_WARN("REC_SHOWCASE set but no worldspaces to fly over");
      }
    }
  }
  cam_time_ += dt;

  if (cam_showcase_) {
    f32 prev = cam_time_ - dt;
    ShowcasePose p = showcase_.Sample(cam_time_);
    LookCameraAt(p.eye, p.target);
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
        renderer_.CaptureScreenshot(path);
        REC_INFO("showcase capture: {}", path);
      }
    }
    if (!showcase_done_) {
      // Benchmark steady-state render: skip the first second of warmup and any
      // half-second-plus frame (cold streaming/IO hitches, not GPU time). Real
      // mid-flight stutter (down to ~2 fps) is still counted, so it shows up.
      if (dt > 0 && dt < 0.5f && cam_time_ >= 1.0f) {
        showcase_dt_min_ = std::min(showcase_dt_min_, dt);
        showcase_dt_max_ = std::max(showcase_dt_max_, dt);
        showcase_bench_time_ += dt;
        ++showcase_frames_;
      }
      if (cam_time_ >= showcase_.duration()) {
        showcase_done_ = true;
        f32 avg =
            showcase_frames_ > 0 ? showcase_bench_time_ / static_cast<f32>(showcase_frames_) : 0.0f;
        REC_INFO("showcase done: {} frames over {:.1f}s, avg {:.0f} fps (min {:.0f}, max {:.0f})",
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

void Engine::WalkUpdate(f32 dt, bool allow) {
  const InputState& input = window_->input();
  window_->SetRelativeMouseMode(true);  // FPS-style mouse look in walk mode

  if (allow) {
    // Mouse look plus right-stick look (rate based; invert/sensitivity from config).
    ctx_.cam_yaw += input.mouse_dx * camera_.sensitivity;
    cam_pitch_ = std::clamp(cam_pitch_ - input.mouse_dy * camera_.sensitivity, -1.4f, 1.4f);
    const f32 inv = input_map_.invert_y ? -1.0f : 1.0f;
    ctx_.cam_yaw += actions_.axis(Axis::kLookX) * input_map_.look_sens_pad * dt;
    cam_pitch_ = std::clamp(
        cam_pitch_ - actions_.axis(Axis::kLookY) * input_map_.look_sens_pad * dt * inv, -1.4f, 1.4f);
  }

  // Move relative to where the camera faces (flattened to the ground plane).
  // The move axes blend keyboard and the analog left stick (stick-down = back).
  Vec3 fwd{std::sin(ctx_.cam_yaw), 0, -std::cos(ctx_.cam_yaw)};
  Vec3 right{std::cos(ctx_.cam_yaw), 0, std::sin(ctx_.cam_yaw)};
  Vec3 move{};
  if (allow) {
    move = move + fwd * (-actions_.axis(Axis::kMoveY));
    move = move + right * actions_.axis(Axis::kMoveX);
  }
  f32 speed = (allow && actions_.down(Action::kSprint)) ? 4.8f : 1.8f;
  if (ctx_.auto_walk) {
    // Test hook: head for the active quest marker / guide mark when one is set,
    // so the guided playthrough follows the quest; otherwise coast forward.
    // Route through pathfinding so the player rounds interior walls (the keep)
    // toward the goal rather than pressing straight into them.
    Vec3 ppos;
    if (ctx_.auto_walk_has_goal && actors_->PlayerWorldPos(&ppos)) {
      const Vec3 wp = npc_->PathToward(ppos, ctx_.auto_walk_goal);
      Vec3 to{wp.x - ppos.x, 0, wp.z - ppos.z};
      const f32 len = Length(to);
      move = len > 0.5f ? to * (1.0f / len) : fwd;
    } else {
      move = fwd;
    }
  }
  f32 move_len = Length(move);
  Vec3 velocity{};
  f32 yaw = 0;
  const bool moving = move_len > 0.01f;
  if (moving) {
    // Clamp (not normalize) so partial stick deflection keeps its analog speed
    // while keyboard diagonals stay capped at full speed.
    if (move_len > 1.0f) move = move * (1.0f / move_len);
    velocity = move * speed;
    yaw = std::atan2(move.x, move.z);  // the biped's +Z faces movement
  }
  bool jump = allow && actions_.pressed(Action::kJump);

  // The actor system owns the player capsule; it steps the character controller
  // and returns the body (feet) position for the follow camera.
  Vec3 body{};
  actors_->MovePlayer(velocity, jump, yaw, moving, speed, dt, &body);

  // Melee: a left-click / right-trigger swing strikes the NPC the player faces,
  // so the player can fight (clear a fort, join a battle). Aim is the camera yaw.
  // REC_AUTO_ATTACK swings on a timer for headless playthrough verification.
  static const bool auto_attack = std::getenv("REC_AUTO_ATTACK") != nullptr;
  bool swing = allow && actions_.pressed(Action::kAttack);
  if (auto_attack) {
    auto_attack_timer_ -= dt;
    if (auto_attack_timer_ <= 0.0f) {
      auto_attack_timer_ = 0.7f;
      swing = true;
    }
  }
  if (swing) npc_->PlayerMeleeStrike(body, ctx_.cam_yaw);

  Vec3 cam_fwd{std::cos(cam_pitch_) * std::sin(ctx_.cam_yaw), std::sin(cam_pitch_),
               -std::cos(cam_pitch_) * std::cos(ctx_.cam_yaw)};
  if (ctx_.third_person) {
    Vec3 pivot = body + Vec3{0, 1.5f, 0};
    ctx_.walk_eye = pivot - cam_fwd * 3.2f;
    ctx_.walk_target = pivot;
  } else {
    ctx_.walk_eye = body + Vec3{0, 1.7f, 0};
    ctx_.walk_target = ctx_.walk_eye + cam_fwd;
  }
}

void Engine::ThrowPhysicsCube() {
  if (!physics_.initialized() || !physics_cube_mesh_) return;
  Vec3 forward = camera_.forward();
  Vec3 origin = camera_.position() + forward * 0.8f;
  // Wood-ish density: heavy enough to splash, light enough to float.
  physics::BodyId body =
      physics_.AddDynamicBox(origin, {0.25f, 0.25f, 0.25f}, 350.0f, forward * 14.0f);
  if (!body) return;
  ecs::Entity entity = world_.Create();
  world_.Add(entity, world::Transform{.position = {origin.x, origin.y, origin.z}});
  world_.Add(entity, world::Renderable{physics_cube_mesh_});
  physics_entities_.push_back({body, entity});
  if (window_ && input_map_.rumble) window_->SetRumble(0.35f, 0.7f, 180);  // toss kick
}

}  // namespace rec
