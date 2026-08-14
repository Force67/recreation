#include <base/algorithm.h>
#include <base/memory/move.h>
#include <base/strings/xstring.h>

#include <cstdio>
#include <filesystem>

#include "core/input_bindings.h"
#include "core/log.h"
#include "runtime/app/engine.h"

// Controls config lifecycle and the pause-menu rebinding flow. The InputMap owns
// the bindings and INI I/O (core/input_bindings); this glues it to the engine:
// loads at startup, applies sensitivity/invert to the camera and the lightbar to
// the pad, and runs the Settings sub-view (capture a key/button per row, nudge
// the look sliders, toggle invert, reset, test the rumble + adaptive triggers).
namespace rx {
namespace {

// The curated, rebindable gameplay actions shown in the settings list, in row
// order. Mirrors the eight rebind_<N> rows in runtime/ui/pause_menu.ugui.
struct SettingsRow {
  Action action;
  const char* label;
};
constexpr SettingsRow kRows[] = {
    {Action::kJump, "Jump"},
    {Action::kSprint, "Sprint"},
    {Action::kSneak, "Sneak"},
    {Action::kActivate, "Activate"},
    {Action::kAttack, "Attack"},
    {Action::kReady, "Ready Weapon"},
    {Action::kToggleWalk, "Walk / Fly"},
    {Action::kToggleJournal, "Journal"},
};
constexpr int kRowCount = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));

// Stats counters run to seven figures (3,258,245 gold found in the reference
// save), which is unreadable without grouping.
base::String Grouped(u32 value) {
  char digits[16];
  const int n = std::snprintf(digits, sizeof(digits), "%u", value);
  base::String out;
  for (int i = 0; i < n; ++i) {
    if (i != 0 && (n - i) % 3 == 0)
      out += ',';
    out += digits[i];
  }
  return out;
}

}  // namespace

void Engine::ApplyControls() {
  camera_.sensitivity = input_map_->look_sens_kbm;
  camera_.pad_sensitivity = input_map_->look_sens_pad;
  camera_.invert_y = input_map_->invert_y;
  if (window_)
    window_->SetLedColor(input_map_->led_r, input_map_->led_g, input_map_->led_b);
}

void Engine::LoadControls() {
  // The engine owns no actions; register the game's set (names, folds and
  // default bindings) before any controls.ini overrides them.
  RegisterGameInput(*input_map_);
  controls_path_ = InputMap::DefaultConfigPath();
  if (!controls_path_.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(controls_path_.c_str()).parent_path(),
                                        ec);
    if (input_map_->LoadFromIni(controls_path_.c_str()))
      RX_INFO("controls loaded from {}", controls_path_.c_str());
    else
      RX_INFO("no controls config at {}, using defaults", controls_path_.c_str());
  }
  ApplyControls();
}

void Engine::SaveControls() {
  if (!controls_path_.empty())
    input_map_->SaveToIni(controls_path_.c_str());
}

void Engine::UpdateSettings() {
  if (!window_ || !game_ui_.menu_open()) {
    capturing_row_ = -1;
    return;
  }
  const InputState& in = window_->input();
  const GamepadState& pad = window_->gamepad();

  if (capturing_row_ >= 0 && capturing_row_ < kRowCount) {
    // Awaiting an input to bind. Escape cancels; otherwise the first key, mouse
    // button, or gamepad button becomes the new binding for this row's action.
    Binding b{};
    bool captured = false, cancel = in.key_pressed(Key::kEscape);
    if (!cancel) {
      for (u8 k = 0; !captured && k < static_cast<u8>(Key::kCount); ++k)
        if (in.pressed[k]) {
          b = {SourceKind::kKey, k, 0};
          captured = true;
        }
      for (u8 m = 0; !captured && m < static_cast<u8>(MouseButton::kCount); ++m) {
        if (in.mouse[m] && !capture_prev_mouse_[m]) {
          b = {SourceKind::kMouseButton, m, 0};
          captured = true;
        }
      }
      if (pad.connected)
        for (u8 g = 0; !captured && g < static_cast<u8>(GamepadButton::kCount); ++g)
          if (pad.pressed[g]) {
            b = {SourceKind::kGamepadButton, g, 0};
            captured = true;
          }
    }
    for (u8 m = 0; m < static_cast<u8>(MouseButton::kCount); ++m)
      capture_prev_mouse_[m] = in.mouse[m];
    if (cancel) {
      capturing_row_ = -1;
    } else if (captured) {
      input_map_->Rebind(kRows[capturing_row_].action, b);
      capturing_row_ = -1;
      SaveControls();
    }
  } else {
    const SettingsRequest req = game_ui_.PollSettingsRequest();
    switch (req.kind) {
      case SettingsRequest::Kind::kRebind:
        if (req.row >= 0 && req.row < kRowCount) {
          capturing_row_ = req.row;
          // Treat the mouse as held so the click that opened capture is ignored.
          for (bool& m : capture_prev_mouse_)
            m = true;
        }
        break;
      case SettingsRequest::Kind::kSensKbm:
        input_map_->look_sens_kbm = base::Clamp(
            input_map_->look_sens_kbm + static_cast<f32>(req.delta) * 0.0005f, 0.0005f, 0.01f);
        ApplyControls();
        SaveControls();
        break;
      case SettingsRequest::Kind::kSensPad:
        input_map_->look_sens_pad =
            base::Clamp(input_map_->look_sens_pad + static_cast<f32>(req.delta) * 0.2f, 0.5f, 6.0f);
        ApplyControls();
        SaveControls();
        break;
      case SettingsRequest::Kind::kInvertToggle:
        input_map_->invert_y = !input_map_->invert_y;
        ApplyControls();
        SaveControls();
        break;
      case SettingsRequest::Kind::kReset:
        input_map_->ResetToDefaults();
        ApplyControls();
        SaveControls();
        break;
      case SettingsRequest::Kind::kTestRumble:
        window_->SetRumble(0.5f, 0.85f, 300);
        if (input_map_->adaptive_triggers) {
          TriggerEffect fx;
          fx.type = TriggerEffect::Type::kResistance;
          fx.start = 80;
          fx.strength = 180;
          window_->SetTriggerEffect(true, true, fx);
        }
        break;
      case SettingsRequest::Kind::kNone:
        break;
    }
  }

  // Rebuild the displayed controls from the live InputMap each frame.
  ControlsView view;
  for (int i = 0; i < kRowCount; ++i) {
    ControlsRow row;
    row.label = kRows[i].label;
    if (capturing_row_ == i) {
      row.capturing = true;
      row.binding = "Press input...";
    } else {
      base::String s;
      for (const Binding& b : input_map_->bindings(kRows[i].action)) {
        if (!s.empty())
          s += " / ";
        s += BindingLabel(b);
      }
      row.binding = s.empty() ? "(unbound)" : s;
    }
    view.rows.push_back(base::move(row));
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.4f", input_map_->look_sens_kbm);
  view.sens_kbm = buf;
  std::snprintf(buf, sizeof(buf), "%.1f", input_map_->look_sens_pad);
  view.sens_pad = buf;
  view.invert_y = input_map_->invert_y;
  view.gamepad = pad.connected;
  game_ui_.SetControlsView(view);

  // The Stats page. Nothing in the engine counts these, so only a loaded save
  // ever changes the table and pushing it on the count moving is enough; the
  // alternative is formatting a hundred strings every frame for a page that is
  // usually not even open.
  if (misc_stats_.size() != stats_pushed_) {
    stats_pushed_ = misc_stats_.size();
    StatsView stats;
    stats.rows.reserve(misc_stats_.size());
    for (const world::MiscStat& stat : misc_stats_.all()) {
      StatsRow row;
      row.label = stat.name;
      row.value = Grouped(stat.value);
      stats.rows.push_back(base::move(row));
    }
    game_ui_.SetStatsView(stats);
  }
}

}  // namespace rx
