// GameUi core: lifetime, the per-frame Build that drives every HUD value, and
// the public setters the engine calls. The editor, the menus and the document
// assembly sit in the sibling game_ui_*.cc; Impl is in game_ui_internal.h.

#include "runtime/ui/game_ui_internal.h"

#if defined(RECREATION_HAS_UGUI)

namespace rx {

GameUi::GameUi() : impl_(base::MakeUnique<Impl>()) {}
GameUi::~GameUi() {
  Shutdown();
}

bool GameUi::Initialize(Window& window, render::Renderer& renderer) {
  render::Device* device = renderer.device();
  if (!device || device->is_stub())
    return false;
  // The HUD backend records raw Vulkan; on other backends the HUD is unavailable.
  const render::VulkanHandles vk = render::GetVulkanHandles(*device);
  if (vk.device == VK_NULL_HANDLE)
    return false;

  impl_->host.window_width = static_cast<float>(window.width());
  impl_->host.window_height = static_cast<float>(window.height());

  ugui::UIConfig cfg;
  cfg.draw_data = true;
  cfg.external_window = &impl_->host;
  cfg.width = static_cast<int>(window.width());
  cfg.height = static_cast<int>(window.height());
  // A design resolution below the real one makes ugui scale every px dimension
  // by real/design, so scale N means asking for a viewport N times smaller than
  // we actually have.
  if (UiScale.get() > 1.001f) {
    cfg.scale_mode = ugui::ViewportScaleMode::kHeight;
    cfg.design_width = static_cast<float>(window.width()) / UiScale.get();
    cfg.design_height = static_cast<float>(window.height()) / UiScale.get();
    RX_INFO("ui scale: {:.2f}x (design {:.0f}x{:.0f})", UiScale.get(), cfg.design_width,
            cfg.design_height);
  }
  if (!impl_->ui.Init(cfg)) {
    RX_WARN("ultragui init failed");
    return false;
  }

  if (const char* font_path = FindFont()) {
    impl_->font = impl_->ui.LoadFont(font_path);
    impl_->ui.set_default_font(impl_->font);
    RX_INFO("ultragui font: {}", font_path);
  } else {
    RX_WARN("no ui font found (set RECREATION_UI_FONT), hud text will be blank");
  }
  // A monospace face for the technical layer (load-order indices, ids, paths),
  // selectable in markup as `font: mono`. Optional; absent leaves those on sans.
  if (const char* mono_path = FindMonoFont()) {
    ugui::FontHandle mono = impl_->ui.LoadFont(mono_path);
    if (mono != ugui::kInvalidFont) {
      impl_->ui.builder().RegisterFont("mono", mono);
      RX_INFO("ultragui mono font: {}", mono_path);
    }
  }
  if (const char* bold_path = FindBoldFont()) {
    ugui::FontHandle bold = impl_->ui.LoadFont(bold_path);
    if (bold != ugui::kInvalidFont) {
      impl_->ui.builder().RegisterFont("bold", bold);
      RX_INFO("ultragui bold font: {}", bold_path);
    }
  } else {
    RX_WARN("no static bold face found (set RECREATION_UI_FONT_BOLD); "
            "ui headings will render at regular weight");
  }

  ui::GuiRenderBackend::InitInfo bi;
  bi.instance = vk.instance;
  bi.physical_device = vk.physical_device;
  bi.device = vk.device;
  bi.queue_family = vk.graphics_family;
  bi.queue = vk.graphics_queue;
  bi.color_format = render::GetVkFormat(renderer.swapchain_format());
  bi.frames_in_flight = 2;
  if (!impl_->backend.Init(bi)) {
    RX_WARN("ultragui vulkan backend init failed");
    impl_->ui.Shutdown();
    return false;
  }
  impl_->ui.set_texture_backend(&impl_->backend);

  base::String doc = BuildUi();
  impl_->ui.LoadUiString(doc.c_str(), "hud");
  // Hot reload: when enabled, the .ugui fragments are polled for edits and the
  // tree is rebuilt in place (see GameUi::Build). Off by default.
  impl_->hot_reload = bool(UiHotReload);
  impl_->CaptureFragmentMtimes();
  if (impl_->hot_reload)
    RX_INFO("ui: hot reload on, watching {}", UiDir().string());

  Impl* impl = (impl_ ? &*impl_ : nullptr);
  impl_->ui.input().set_on_click([impl](ugui::wid w, ugui::MouseButton btn) {
    if (btn != ugui::MouseButton::kLeft)
      return;
    if (impl->RouteFirstRunClick(w))
      return;  // the setup wizard owns this click
    if (impl->RouteMainMenuClick(w))
      return;  // the front menu owns this click
    if (impl->RouteEditorClick(w))
      return;  // editor overlay owns this click
    ugui::WidgetNode* n = impl->ui.world().Get<ugui::WidgetNode>(w);
    if (!n)
      return;
    if (n->name == "btn_resume") {
      impl->menu_open = false;
      impl->settings_open = false;
      impl->ApplyMenuVisibility();
    } else if (n->name == "btn_settings") {
      impl->settings_open = true;
      impl->ApplyMenuVisibility();
    } else if (n->name == "btn_settings_back") {
      impl->settings_open = false;
      impl->ApplyMenuVisibility();
    } else if (n->name == "btn_quit") {
      impl->quit_requested = true;
    } else if (n->name.rfind("rebind_", 0) == 0 && n->name.find('_', 7) == base::String::npos) {
      // A rebind row (rebind_<N>): ask the engine to capture the next input.
      impl->settings_request = {SettingsRequest::Kind::kRebind, std::atoi(n->name.c_str() + 7), 0};
    } else if (n->name == "btn_skbm_dec") {
      impl->settings_request = {SettingsRequest::Kind::kSensKbm, 0, -1};
    } else if (n->name == "btn_skbm_inc") {
      impl->settings_request = {SettingsRequest::Kind::kSensKbm, 0, +1};
    } else if (n->name == "btn_spad_dec") {
      impl->settings_request = {SettingsRequest::Kind::kSensPad, 0, -1};
    } else if (n->name == "btn_spad_inc") {
      impl->settings_request = {SettingsRequest::Kind::kSensPad, 0, +1};
    } else if (n->name == "btn_invert") {
      impl->settings_request = {SettingsRequest::Kind::kInvertToggle, 0, 0};
    } else if (n->name == "btn_reset") {
      impl->settings_request = {SettingsRequest::Kind::kReset, 0, 0};
    } else if (n->name == "btn_rumble") {
      impl->settings_request = {SettingsRequest::Kind::kTestRumble, 0, 0};
    }
  });

  // Editor + character-creation overlays start collapsed; the engine reveals them.
  impl_->SetVisible("editor_root", false);
  impl_->SetVisible("cg_root", false);

  // Debug aid: RECREATION_UI_MENU opens the pause menu at startup,
  // RECREATION_UI_MENU_SETTINGS opens it on the controls sub-view.
  if (UiMenu || UiMenuSettings)
    impl_->menu_open = true;
  if (UiMenuSettings)
    impl_->settings_open = true;
  impl_->ApplyMenuVisibility();  // menu starts hidden unless forced open
  // Debug aid: RECREATION_MAIN_MENU opens the NEXUS front menu at startup.
  if (MainMenu)
    impl_->main_menu_open = true;
  impl_->ApplyMainMenu();
  // Debug aid: RECREATION_FIRST_RUN opens the setup wizard at startup.
  if (FirstRun)
    impl_->first_run_open = true;
  impl_->ApplyFirstRun();
  impl_->initialized = true;
  RX_INFO("ultragui hud initialized (draw-data mode)");
  return true;
}

void GameUi::Shutdown() {
  if (!impl_ || !impl_->initialized)
    return;
  impl_->backend.Shutdown();
  impl_->ui.Shutdown();
  impl_->initialized = false;
}

void GameUi::ToggleMenu() {
  if (!impl_->initialized)
    return;
  impl_->menu_open = !impl_->menu_open;
  impl_->settings_open = false;  // always reopen on the main pause screen
  impl_->ApplyMenuVisibility();
}

bool GameUi::menu_open() const {
  return impl_->initialized && impl_->menu_open;
}
bool GameUi::quit_requested() const {
  return impl_->initialized && impl_->quit_requested;
}

void GameUi::OpenMainMenu() {
  if (!impl_->initialized)
    return;
  impl_->main_menu_open = true;
  impl_->mm_screen = 0;
  impl_->ApplyMainMenu();
}

void GameUi::CloseMainMenu() {
  if (!impl_->initialized)
    return;
  impl_->main_menu_open = false;
  impl_->ApplyMainMenu();
}

bool GameUi::main_menu_open() const {
  return impl_->initialized && impl_->main_menu_open;
}

void GameUi::MainMenuMove(int dx, int dy) {
  if (!impl_->initialized || !impl_->main_menu_open || impl_->mm_screen != 0)
    return;
  if (dy)
    impl_->mm_nav = (impl_->mm_nav + dy + kMenuNavItems) % kMenuNavItems;
  if (dx)
    impl_->mm_universe = base::Clamp(impl_->mm_universe + dx, 0, kMenuUniverses - 1);
}

void GameUi::MainMenuActivate() {
  if (!impl_->initialized || !impl_->main_menu_open || impl_->mm_screen != 0)
    return;
  impl_->ActivateNav();
}

bool GameUi::MainMenuBack() {
  if (!impl_->initialized || !impl_->main_menu_open)
    return false;
  if (impl_->mm_screen != 0) {
    impl_->mm_screen = 0;
    return true;
  }
  return false;
}

bool GameUi::MainMenuAtRoot() const {
  return impl_->initialized && impl_->main_menu_open && impl_->mm_screen == 0;
}

void GameUi::SetMainMenuUniverses(const base::Vector<base::String>& names,
                                  const base::Vector<bool>& available) {
  if (!impl_->initialized)
    return;
  if (!names.empty())
    impl_->mm_universe_names = names;
  if (!available.empty())
    impl_->mm_available = available;
}

void GameUi::SetMainMenuBackdrop(int universe, u64 texture) {
  if (!impl_->initialized || universe < 0 || universe >= kMenuUniverses)
    return;
  impl_->mm_backdrop[universe] = texture;
}

void GameUi::SetMainMenuGlyph(const base::String& widget, u64 texture) {
  if (!impl_->initialized)
    return;
  for (auto& [name, tex] : impl_->mm_glyphs)
    if (name == widget) {
      tex = texture;
      return;
    }
  impl_->mm_glyphs.emplace_back(widget, texture);
}

void GameUi::SetMainMenuStats(const MainMenuStats& stats) {
  if (impl_->initialized)
    impl_->mm_stats = stats;
}

void GameUi::SetMainMenuMods(const base::Vector<base::String>& mods) {
  if (impl_->initialized)
    impl_->mm_mods = mods;
}

void GameUi::SetMainMenuNews(const base::Vector<MenuNewsItem>& news) {
  if (impl_->initialized)
    impl_->mm_news = news;
}

int GameUi::selected_universe() const {
  return impl_->initialized ? impl_->mm_universe : 0;
}

MainMenuRequest GameUi::PollMainMenuRequest() {
  MainMenuRequest r;
  if (impl_->initialized) {
    r = impl_->mm_request;
    impl_->mm_request = MainMenuRequest{};  // consume
  }
  return r;
}

void GameUi::OpenFirstRun() {
  if (!impl_->initialized)
    return;
  impl_->first_run_open = true;
  impl_->fr_step = 0;
  impl_->fr_dropdown = -1;
  // Debug aid: RECREATION_FIRST_RUN_STEP=<0..4> opens the wizard on that page
  // (so a headless capture can grab any page, not just the welcome screen).
  if (const char* s = FirstRunStep.get()) {
    const int v = std::atoi(s);
    if (v >= 0 && v < kFirstRunSteps)
      impl_->fr_step = v;
  }
  impl_->ApplyFirstRun();
}

void GameUi::CloseFirstRun() {
  if (!impl_->initialized)
    return;
  impl_->first_run_open = false;
  impl_->ApplyFirstRun();
}

bool GameUi::first_run_open() const {
  return impl_->initialized && impl_->first_run_open;
}

void GameUi::FirstRunNext() {
  if (impl_->initialized && impl_->first_run_open)
    impl_->AdvanceFirstRun();
}

void GameUi::FirstRunBack() {
  if (impl_->initialized && impl_->first_run_open)
    impl_->RetreatFirstRun();
}

void GameUi::SetFirstRunView(const FirstRunView& view) {
  if (impl_->initialized)
    impl_->fr_view = view;
}

FirstRunRequest GameUi::PollFirstRunRequest() {
  FirstRunRequest r;
  if (impl_->initialized) {
    r = impl_->fr_request;
    impl_->fr_request = FirstRunRequest{};  // consume
  }
  return r;
}

bool GameUi::settings_open() const {
  return impl_->initialized && impl_->settings_open;
}

void GameUi::SetControlsView(const ControlsView& view) {
  if (!impl_->initialized)
    return;
  Impl* impl = (impl_ ? &*impl_ : nullptr);
  for (size_t i = 0; i < view.rows.size(); ++i) {
    const base::String base = "rebind_" + base::ToString(i);
    impl->SetText((base + "_lbl").c_str(), view.rows[i].label.c_str());
    impl->SetText((base + "_key").c_str(), view.rows[i].binding.c_str());
  }
  impl->SetText("sens_kbm_val", view.sens_kbm.c_str());
  impl->SetText("sens_pad_val", view.sens_pad.c_str());
  impl->SetText("btn_invert", view.invert_y ? "Invert Y: On" : "Invert Y: Off");
  impl->SetVisible("btn_rumble", view.gamepad);  // rumble test only with a pad
}

SettingsRequest GameUi::PollSettingsRequest() {
  SettingsRequest r;
  if (impl_->initialized) {
    r = impl_->settings_request;
    impl_->settings_request = SettingsRequest{};  // consume
  }
  return r;
}

void GameUi::SetQuest(const HudQuest& quest) {
  if (impl_->initialized)
    impl_->quest = quest;
}

void GameUi::SetChatLines(const base::Vector<base::String>& lines) {
  if (impl_->initialized)
    impl_->chat_lines = lines;
}

void GameUi::SetScoreboard(bool open,
                           const base::String& title,
                           const base::String& header,
                           const base::Vector<base::String>& rows) {
  if (!impl_->initialized)
    return;
  impl_->scoreboard_open = open;
  impl_->scoreboard_title = title;
  impl_->scoreboard_header = header;
  impl_->scoreboard_rows = rows;
}

void GameUi::SetPrompts(const base::Vector<base::String>& prompts) {
  if (impl_->initialized)
    impl_->mp_prompts = prompts;
}

void GameUi::SetCompassBlips(const base::Vector<CompassBlip>& blips) {
  if (impl_->initialized)
    impl_->compass_blips = blips;
}

void GameUi::SetNametags(const base::Vector<Nametag>& nametags) {
  if (impl_->initialized)
    impl_->nametags = nametags;
}

void GameUi::SetHudGauges(const base::Vector<HudGauge>& gauges) {
  if (impl_->initialized)
    impl_->hud_gauges = gauges;
}

void GameUi::FlashQuestUpdate(const base::String& message) {
  if (!impl_->initialized)
    return;
  impl_->toast_text = message;
  impl_->toast_age = 0.0f;
}

void GameUi::SetActivatePrompt(const base::String& prompt) {
  if (impl_->initialized)
    impl_->activate_prompt = prompt;
}

void GameUi::SetHudVisible(bool visible) {
  if (!impl_->initialized)
    return;
  impl_->SetVisible("topbar", visible);  // compass
  impl_->SetVisible("crosshair", visible);
  impl_->SetVisible("vitals", visible);   // health / magicka / stamina bars
  impl_->SetVisible("readout", visible);  // fps / coords / heading
}

void GameUi::SetObjectiveMarker(bool active, float bearing_deg, float distance_m) {
  if (!impl_->initialized)
    return;
  impl_->marker_active = active;
  impl_->marker_bearing = bearing_deg;
  impl_->marker_distance = distance_m;
}

void GameUi::SetDialogue(const DialogueView& dialogue) {
  if (impl_->initialized)
    impl_->dialogue = dialogue;
}

void GameUi::SetContainer(const ContainerView& container) {
  if (impl_->initialized)
    impl_->container = container;
}

void GameUi::SetWarMap(bool open,
                       const base::Vector<WarHoldEntry>& holds,
                       float imperial_fraction) {
  if (!impl_->initialized)
    return;
  impl_->war_map_open = open;
  impl_->war_holds = holds;
  impl_->war_progress = imperial_fraction;
}

void GameUi::SetJournal(bool open, const base::Vector<HudQuest>& quests, int selected) {
  if (!impl_->initialized)
    return;
  impl_->journal_open = open;
  impl_->journal = quests;
  impl_->journal_selected = selected;
}

void GameUi::SetEditorView(const EditorView& view) {
  if (impl_->initialized)
    impl_->editor = view;
}

void GameUi::SetEditorEventSink(base::Function<void(const EditorUiEvent&)> sink) {
  if (impl_->initialized)
    impl_->editor_sink = base::move(sink);
}

void GameUi::ScalePointer(f32 window_x, f32 window_y, f32* canvas_x, f32* canvas_y) const {
  if (canvas_x)
    *canvas_x = window_x * impl_->pointer_scale_x;
  if (canvas_y)
    *canvas_y = window_y * impl_->pointer_scale_y;
}

void GameUi::SetCharGenView(const CharGenView& view) {
  if (impl_->initialized)
    impl_->chargen = view;
}

u64 GameUi::CreateUiTexture(int width, int height, const u8* rgba) {
  if (!impl_->initialized || !rgba || width <= 0 || height <= 0)
    return 0;
  return impl_->backend.CreateTexture(static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                      ugui::RHIFormat::kRgba8Unorm, rgba, ugui::RHIFilter::kLinear);
}

void GameUi::Build(Window& window,
                   render::Renderer& renderer,
                   FlyCamera& camera,
                   f32 frame_delta,
                   render::FrameView* view) {
  if (!impl_->initialized)
    return;
  Impl* impl = (impl_ ? &*impl_ : nullptr);

  // Hot reload: poll the .ugui fragments a few times a second and rebuild the
  // tree in place when one is edited. Gated on RECREATION_UI_HOT_RELOAD.
  if (impl->hot_reload) {
    impl->reload_timer += frame_delta;
    if (impl->reload_timer >= 0.25f) {
      impl->reload_timer = 0.0f;
      if (impl->FragmentsChanged())
        impl->ReloadUi();
    }
  }

  // Size the UI canvas to the actual backbuffer (swapchain) extent, not the
  // window size; they differ on HiDPI / when the swapchain is clamped, which
  // otherwise lays the menu out over only part of the screen (a black bar).
  const float fb_w = static_cast<float>(renderer.output_width());
  const float fb_h = static_cast<float>(renderer.output_height());
  impl->host.window_width = fb_w > 0.f ? fb_w : static_cast<float>(window.width());
  impl->host.window_height = fb_h > 0.f ? fb_h : static_cast<float>(window.height());

  // Feed the per-frame input snapshot into ultragui's queue, scaling the cursor
  // from window space into the (possibly larger) backbuffer canvas so clicks
  // line up with the widgets.
  const InputState& in = window.input();
  ugui::InputQueue& q = impl->ui.platform()->input_queue();
  const float msx = window.width() > 0 ? fb_w / static_cast<float>(window.width()) : 1.f;
  const float msy = window.height() > 0 ? fb_h / static_cast<float>(window.height()) : 1.f;
  impl->pointer_scale_x = msx;
  impl->pointer_scale_y = msy;
  // A finger drives the same pointer the mouse does. This is what makes the HUD
  // usable on a handheld: with touch.emits_mouse off (the steamdeck profile) SDL
  // no longer synthesizes a cursor, so without this the panel does nothing. The
  // decision lives in touch_pointer.h so it is testable without a panel.
  const TouchPointerEvents touch_events =
      ResolveTouchPointer(window.touch(), impl->touch_pointer, msx, msy);

  if (touch_events.move)
    q.PushMove({touch_events.x, touch_events.y});
  if (touch_events.button)
    q.PushButton(ugui::MouseButton::kLeft, touch_events.down);
  if (touch_events.scroll)
    q.PushScroll({0.0f, touch_events.scroll_y});

  if (touch_events.use_mouse) {
    q.PushMove({in.mouse_x * msx, in.mouse_y * msy});
    const ugui::MouseButton buttons[3] = {ugui::MouseButton::kLeft, ugui::MouseButton::kRight,
                                          ugui::MouseButton::kMiddle};
    const MouseButton rec_buttons[3] = {MouseButton::kLeft, MouseButton::kRight,
                                        MouseButton::kMiddle};
    for (int i = 0; i < 3; ++i) {
      bool down = in.button(rec_buttons[i]);
      if (down != impl->prev_mouse[i])
        q.PushButton(buttons[i], down);
      impl->prev_mouse[i] = down;
    }
    if (in.wheel != 0.0f)
      q.PushScroll({0.0f, in.wheel});
  }

  // Feed gamepad + keyboard navigation into ugui's focus ring so menus with
  // tab-index'd widgets (pause / settings) are navigable by pad and keyboard.
  // ugui drives nav/activation internally from these queued events.
  const GamepadState& pad = window.gamepad();
  if (pad.connected) {
    // Map our buttons to ugui's (the enums differ in order); skip unmapped ones.
    static constexpr int kNoUgui = -1;
    auto to_ugui = [](GamepadButton b) -> int {
      switch (b) {
        case GamepadButton::kSouth:
          return static_cast<int>(ugui::GamepadButton::kA);
        case GamepadButton::kEast:
          return static_cast<int>(ugui::GamepadButton::kB);
        case GamepadButton::kWest:
          return static_cast<int>(ugui::GamepadButton::kX);
        case GamepadButton::kNorth:
          return static_cast<int>(ugui::GamepadButton::kY);
        case GamepadButton::kBack:
          return static_cast<int>(ugui::GamepadButton::kBack);
        case GamepadButton::kGuide:
          return static_cast<int>(ugui::GamepadButton::kGuide);
        case GamepadButton::kStart:
          return static_cast<int>(ugui::GamepadButton::kStart);
        case GamepadButton::kLeftStick:
          return static_cast<int>(ugui::GamepadButton::kLeftThumb);
        case GamepadButton::kRightStick:
          return static_cast<int>(ugui::GamepadButton::kRightThumb);
        case GamepadButton::kLeftShoulder:
          return static_cast<int>(ugui::GamepadButton::kLeftBumper);
        case GamepadButton::kRightShoulder:
          return static_cast<int>(ugui::GamepadButton::kRightBumper);
        case GamepadButton::kDpadUp:
          return static_cast<int>(ugui::GamepadButton::kDPadUp);
        case GamepadButton::kDpadDown:
          return static_cast<int>(ugui::GamepadButton::kDPadDown);
        case GamepadButton::kDpadLeft:
          return static_cast<int>(ugui::GamepadButton::kDPadLeft);
        case GamepadButton::kDpadRight:
          return static_cast<int>(ugui::GamepadButton::kDPadRight);
        default:
          return kNoUgui;
      }
    };
    for (int b = 0; b < static_cast<int>(GamepadButton::kCount); ++b) {
      bool down = pad.buttons[b];
      if (down == impl->prev_pad[b])
        continue;
      impl->prev_pad[b] = down;
      int u = to_ugui(static_cast<GamepadButton>(b));
      if (u != kNoUgui)
        q.PushGamepadButton(static_cast<ugui::GamepadButton>(u), down);
    }
    // The stick axes drive repeat navigation; our GamepadAxis order matches ugui's.
    q.PushGamepadAxis(ugui::GamepadAxis::kLeftX, pad.axis(GamepadAxis::kLeftX));
    q.PushGamepadAxis(ugui::GamepadAxis::kLeftY, pad.axis(GamepadAxis::kLeftY));
  }
  // Keyboard focus nav: Tab cycles, Enter/Space activate (ugui uses GLFW codes).
  const int shift_mod = in.key(Key::kLeftShift) ? 0x0001 : 0;
  if (in.key_pressed(Key::kTab))
    q.PushKey(258, 0, true, false, shift_mod);
  if (in.key_pressed(Key::kReturn))
    q.PushKey(257, 0, true, false, 0);

  // --- Drive HUD values from real engine state ---
  // Compass heading from the camera's facing direction.
  Vec3 fwd = camera.forward();
  float heading = std::atan2(fwd.x, -fwd.z) * 57.29578f;
  if (heading < 0.0f)
    heading += 360.0f;
  impl->SetStyleField(
      "compass_strip", [](ugui::Style& s, float v) { s.left_offset = ugui::Length::Px(v); },
      CompassStripLeft(heading));

  // Direction is spelled out ("NE  120 m") rather than drawn, the compass
  // having no pips. SetCompassBlips still accepts blips; nothing renders them.
  impl->SetVisible("quest_marker_box", impl->marker_active);
  if (impl->marker_active) {
    const float abs_bearing =
        std::fmod(heading + impl->marker_bearing + 360.0f, 360.0f);
    const char* toward =
        kCardinals[static_cast<int>(std::fmod(abs_bearing + 22.5f, 360.0f) / 45.0f) % 8];
    char mbuf[64];
    std::snprintf(mbuf, sizeof(mbuf), "%s   %.0f m", toward, impl->marker_distance);
    impl->SetText("quest_marker_text", mbuf);
  }

  // Stamina drains while sprinting (shift + movement), regenerates otherwise.
  bool moving = in.key(Key::kW) || in.key(Key::kA) || in.key(Key::kS) || in.key(Key::kD);
  bool sprinting = in.key(Key::kLeftShift) && moving;
  impl->stamina += (sprinting ? -0.45f : 0.30f) * frame_delta;
  impl->stamina = base::Clamp(impl->stamina, 0.0f, 1.0f);
  impl->SetStyleField(
      "bar_stamina_fill", [](ugui::Style& s, float v) { s.width = ugui::Length::Pct(v); },
      impl->stamina * 100.0f);

  // Readout text.
  char buf[160];
  impl->last_fps = static_cast<int>(frame_delta > 0 ? 1.0f / frame_delta + 0.5f : 0.0f);
  std::snprintf(buf, sizeof(buf), "%.0f fps", frame_delta > 0 ? 1.0f / frame_delta : 0.0f);
  impl->SetText("hud_fps", buf);
  Vec3 pos = camera.position();
  std::snprintf(buf, sizeof(buf), "x %.0f   y %.0f   z %.0f", pos.x, pos.y, pos.z);
  impl->SetText("hud_coords", buf);
  const char* card = kCardinals[static_cast<int>(std::fmod(heading + 22.5f, 360.0f) / 45.0f) % 8];
  std::snprintf(buf, sizeof(buf), "%s  %.0f deg", card, heading);
  impl->SetText("hud_heading", buf);
  std::snprintf(buf, sizeof(buf), "%d", impl->mm_stats.gold);
  impl->SetText("hud_gold", buf);

  // Which world you are in, said in one colour.
  impl->ApplyDomainAccent(AccentForUniverse(impl->mm_stats.universe));

  // The pause menu's detail pane, from the same stats the front-end reads.
  {
    const MainMenuStats& st = impl->mm_stats;
    impl->SetText("menu_pname", st.player_name.empty() ? st.account : st.player_name);
    impl->SetText("menu_where", st.universe);
    impl->SetText("menu_level", base::ToString(st.level));
    impl->SetText("menu_quests", base::ToString(st.active_quests));
    impl->SetText("menu_gold", base::ToString(st.gold));
    impl->SetText("menu_loc", st.location.empty() ? "-" : st.location);
    impl->SetText("menu_build",
                  st.build.empty() ? base::String("Recreation") : ("Recreation  " + st.build));
    impl->SetStyleField(
        "menu_hp_fill", [](ugui::Style& s, float v) { s.width = ugui::Length::Pct(v); },
        base::Clamp(st.health, 0.0f, 1.0f) * 100.0f);
    impl->SetStyleField(
        "menu_mp_fill", [](ugui::Style& s, float v) { s.width = ugui::Length::Pct(v); },
        base::Clamp(st.magicka, 0.0f, 1.0f) * 100.0f);
    impl->SetStyleField(
        "menu_sp_fill", [](ugui::Style& s, float v) { s.width = ugui::Length::Pct(v); },
        base::Clamp(st.stamina, 0.0f, 1.0f) * 100.0f);
  }

  // Area-title reveal: on change only, then times out.
  if (impl->mm_stats.location != impl->loc_shown) {
    impl->loc_shown = impl->mm_stats.location;
    impl->loc_reveal_age = 0.0f;
  }
  impl->loc_reveal_age += frame_delta;
  const bool loc_on =
      impl->loc_reveal_age < kLocationRevealSeconds && !impl->loc_shown.empty();
  impl->SetVisible("loc_box", loc_on);
  if (loc_on)
    impl->SetText("loc_name", impl->loc_shown.c_str());

  // Managed gameplay gauges (oxygen, radiation, ...): the pooled labeled bar
  // stack above the vitals, one row per active gauge.
  for (int i = 0; i < kHudGaugeRows; ++i) {
    const base::String row = "hud_gauge" + base::ToString(i);
    if (i < static_cast<int>(impl->hud_gauges.size())) {
      const HudGauge& g = impl->hud_gauges[i];
      impl->SetVisible(row.c_str(), true);
      impl->SetText((row + "_lbl").c_str(), g.label.c_str());
      impl->SetStyleField((row + "_fill").c_str(),
                          [](ugui::Style& s, float v) { s.width = ugui::Length::Pct(v); },
                          base::Clamp(g.fraction, 0.0f, 1.0f) * 100.0f);
      // White default keeps an unstyled gauge on-palette; a module's own
      // colour wins.
      impl->SetBackground((row + "_fill").c_str(), Rgba(g.color ? g.color : 0xffffffffu));
    } else {
      impl->SetVisible(row.c_str(), false);
    }
  }

  // Multiplayer chat box: the last kChatRows lines, newest at the bottom. The
  // whole box collapses when there is nothing to show.
  const bool chat_on = !impl->chat_lines.empty();
  impl->SetVisible("chat_box", chat_on);
  if (chat_on) {
    const int count = static_cast<int>(impl->chat_lines.size());
    const int first = base::Max(0, count - kChatRows);  // tail window
    for (int i = 0; i < kChatRows; ++i) {
      const base::String row = "chat_line" + base::ToString(i);
      const int src = first + i;
      if (src < count) {
        impl->SetText(row.c_str(), impl->chat_lines[src].c_str());
        impl->SetVisible(row.c_str(), true);
      } else {
        impl->SetVisible(row.c_str(), false);
      }
    }
  }

  // Multiplayer scoreboard: a centered panel of player rows, shown while open.
  impl->SetVisible("scoreboard_box", impl->scoreboard_open);
  if (impl->scoreboard_open) {
    impl->SetText("scoreboard_title",
                  impl->scoreboard_title.empty() ? "Players" : impl->scoreboard_title.c_str());
    impl->SetText("scoreboard_header", impl->scoreboard_header.c_str());
    for (int i = 0; i < kScoreRows; ++i) {
      const base::String row = "scoreboard_row" + base::ToString(i);
      if (static_cast<size_t>(i) < impl->scoreboard_rows.size()) {
        impl->SetText(row.c_str(), impl->scoreboard_rows[i].c_str());
        impl->SetVisible(row.c_str(), true);
      } else {
        impl->SetVisible(row.c_str(), false);
      }
    }
  }

  // Multiplayer interaction prompts: a small bottom-centre stack, shown while any
  // are active.
  const bool prompts_on = !impl->mp_prompts.empty();
  impl->SetVisible("mp_prompt_box", prompts_on);
  if (prompts_on) {
    for (int i = 0; i < kPromptRows; ++i) {
      const base::String row = "mp_prompt" + base::ToString(i);
      if (static_cast<size_t>(i) < impl->mp_prompts.size()) {
        impl->SetText(row.c_str(), impl->mp_prompts[i].c_str());
        impl->SetVisible(row.c_str(), true);
      } else {
        impl->SetVisible(row.c_str(), false);
      }
    }
  }

  // Floating world-space nametags: place each label at its screen position. The
  // engine projects the world position; we centre the pill on it (a rough half
  // width per character) and bias it up so it floats above the player.
  for (int i = 0; i < kNametags; ++i) {
    const base::String tag = "nametag" + base::ToString(i);
    if (static_cast<size_t>(i) < impl->nametags.size()) {
      const GameUi::Nametag& n = impl->nametags[i];
      impl->SetText((tag + "_txt").c_str(), n.label.c_str());
      const float half_w = 7.0f + n.label.size() * 3.7f;  // approx half the pill width
      impl->SetStyleField(
          tag.c_str(), [](ugui::Style& s, float v) { s.left_offset = ugui::Length::Px(v); },
          n.sx - half_w);
      impl->SetStyleField(
          tag.c_str(), [](ugui::Style& s, float v) { s.top = ugui::Length::Px(v); }, n.sy);
      impl->SetVisible(tag.c_str(), true);
    } else {
      impl->SetVisible(tag.c_str(), false);
    }
  }

  // --- Quest HUD ---
  const bool has_quest = !impl->quest.title.empty();
  impl->SetVisible("questtracker", has_quest);
  if (has_quest)
    impl->SetText("quest_title", impl->quest.title.c_str());
  for (int i = 0; i < kQuestObjectiveRows; ++i) {
    base::String row = "quest_obj" + base::ToString(i);
    if (has_quest && static_cast<size_t>(i) < impl->quest.objectives.size()) {
      const HudQuest::Objective& o = impl->quest.objectives[i];
      // A check for done objectives, a bullet for the rest.
      base::String line = (o.completed ? "✓  " : "•  ") + o.text;
      impl->SetText(row.c_str(), line.c_str());
      impl->SetVisible(row.c_str(), true);
    } else {
      impl->SetVisible(row.c_str(), false);
    }
  }

  // The "quest updated" banner fades out after a few seconds.
  impl->toast_age += frame_delta;
  const bool toast_on = impl->toast_age < kToastSeconds && !impl->toast_text.empty();
  impl->SetVisible("quest_toast_box", toast_on);
  if (toast_on)
    impl->SetText("quest_toast", impl->toast_text.c_str());

  // Centered activation prompt ("Talk to Ralof", "Open the gate", ...).
  const bool prompt_on = !impl->activate_prompt.empty();
  impl->SetVisible("activate_box", prompt_on);
  if (prompt_on)
    impl->SetText("activate_prompt", impl->activate_prompt.c_str());

  // Dialogue panel: speaker + last NPC line + numbered player topics.
  const DialogueView& dlg = impl->dialogue;
  impl->SetVisible("dialogue_box", dlg.open);
  if (dlg.open) {
    impl->SetText("dialogue_speaker", dlg.speaker.c_str());
    impl->SetText("dialogue_npc", dlg.npc_line.c_str());
    for (int i = 0; i < kDialogueOptionRows; ++i) {
      const base::String row = "dialogue_opt" + base::ToString(i);
      if (i < static_cast<int>(dlg.options.size())) {
        const base::String line = base::ToString(i + 1) + ". " + dlg.options[i];
        impl->SetText(row.c_str(), line.c_str());
        impl->SetVisible(row.c_str(), true);
      } else {
        impl->SetVisible(row.c_str(), false);
      }
    }
  }

  // Container loot panel: the container's name and a fixed pool of item rows.
  const ContainerView& cont = impl->container;
  impl->SetVisible("container_box", cont.open);
  if (cont.open) {
    impl->SetText("container_head", cont.name.c_str());
    for (int i = 0; i < kContainerRows; ++i) {
      const base::String row = "container_item" + base::ToString(i);
      if (i < static_cast<int>(cont.items.size())) {
        base::String line = cont.items[i].name;
        if (cont.items[i].count > 1)
          line += "  x" + base::ToString(cont.items[i].count);
        impl->SetText(row.c_str(), line.c_str());
        impl->SetVisible(row.c_str(), true);
      } else {
        impl->SetVisible(row.c_str(), false);
      }
    }
    // An empty chest still gets a line so it does not read as a bug.
    if (cont.items.empty()) {
      impl->SetText("container_item0", "(empty)");
      impl->SetVisible("container_item0", true);
    }
  }

  // Quest journal: a numbered list of active quests; an arrow marks the tracked
  // one and its objectives are listed below.
  impl->SetVisible("journal_box", impl->journal_open);
  if (impl->journal_open) {
    for (int i = 0; i < kJournalRows; ++i) {
      const base::String row = "journal_q" + base::ToString(i);
      if (i < static_cast<int>(impl->journal.size())) {
        const base::String mark = i == impl->journal_selected ? "▶ " : "   ";
        const base::String line = mark + base::ToString(i + 1) + ". " + impl->journal[i].title;
        impl->SetText(row.c_str(), line.c_str());
        impl->SetVisible(row.c_str(), true);
      } else {
        impl->SetVisible(row.c_str(), false);
      }
    }
    const HudQuest* sel = (impl->journal_selected >= 0 &&
                           impl->journal_selected < static_cast<int>(impl->journal.size()))
                              ? &impl->journal[impl->journal_selected]
                              : nullptr;
    for (int i = 0; i < kJournalObjRows; ++i) {
      const base::String row = "journal_obj" + base::ToString(i);
      if (sel && i < static_cast<int>(sel->objectives.size())) {
        const base::String line =
            (sel->objectives[i].completed ? "✓  " : "•  ") + sel->objectives[i].text;
        impl->SetText(row.c_str(), line.c_str());
        impl->SetVisible(row.c_str(), true);
      } else {
        impl->SetVisible(row.c_str(), false);
      }
    }
  }

  // War-map overlay: the Civil War campaign board, each hold and its owner,
  // coloured by side, with the overall war-progress bar.
  impl->SetVisible("war_map_box", impl->war_map_open);
  if (impl->war_map_open) {
    int imperial = 0, stormcloak = 0;
    for (int i = 0; i < kWarHoldRows; ++i) {
      const base::String row = "war_hold" + base::ToString(i);
      if (i < static_cast<int>(impl->war_holds.size())) {
        const GameUi::WarHoldEntry& h = impl->war_holds[i];
        const char* owner = h.owner == 1 ? "Imperial" : h.owner == 2 ? "Stormcloak" : "Contested";
        // Three states on three values already in the palette.
        const ugui::Color col = h.owner == 1   ? Rgba(0xffffffffu)
                                : h.owner == 2 ? Rgba(0xff2e17ffu)
                                               : Rgba(0xffffff66u);
        impl->SetText(row.c_str(), (h.name + "  --  " + owner).c_str());
        impl->SetTextColor(row.c_str(), col);
        impl->SetVisible(row.c_str(), true);
        if (h.owner == 1)
          ++imperial;
        if (h.owner == 2)
          ++stormcloak;
      } else {
        impl->SetVisible(row.c_str(), false);
      }
    }
    char sub[96];
    std::snprintf(sub, sizeof(sub), "Imperial Legion %d   |   Stormcloaks %d", imperial,
                  stormcloak);
    impl->SetText("war_map_sub", sub);
    impl->SetStyleField(
        "war_bar_fill", [](ugui::Style& s, float v) { s.width = ugui::Length::Pct(v); },
        base::Clamp(impl->war_progress, 0.0f, 1.0f) * 100.0f);
    char prog[96];
    std::snprintf(prog, sizeof(prog), "Imperial control: %d%%",
                  static_cast<int>(base::Clamp(impl->war_progress, 0.0f, 1.0f) * 100.0f + 0.5f));
    impl->SetText("war_map_progress", prog);
  }

  // Map editor overlay (asset browser, toolbar, inspector, status, reticle).
  impl->ApplyEditorView();

  // Character-creation overlay (race/sex/preset docks + slider list).
  impl->ApplyCharGenView();

  // NEXUS main menu (the startup front screen, on top of everything).
  impl->ApplyMainMenu();

  // First-run setup wizard (the out-of-box experience, above even the menu).
  impl->ApplyFirstRun();

  // Produce the draw list (input routing + layout + paint, no GPU work).
  const ugui::DrawData& dd = impl->ui.RenderDrawData();
  impl->draw_data = &dd;

  // Tell the renderer whether any widget wants backdrop blur this frame, so it
  // only captures + blurs the backbuffer when a frosted panel is actually shown.
  view->needs_blur = false;
  for (u32 i = 0; i < dd.command_count; ++i) {
    if (dd.commands[i].blur > 0.0f) {
      view->needs_blur = true;
      break;
    }
  }

  // Upload the glyph atlas if it grew this frame.
  if (impl->ui.text_engine().atlas_revision() != impl->font_revision) {
    ugui::Vec2 as = impl->ui.text_engine().atlas_size();
    impl->backend.UpdateFontAtlas(impl->ui.text_engine().atlas_pixels(), static_cast<u32>(as.x),
                                  static_cast<u32>(as.y));
    impl->font_revision = impl->ui.text_engine().atlas_revision();
  }

  impl->backend.NewFrame();
  view->hud_draw = [impl, view](render::CommandList& cmd) {
    // The renderer fills view->blur_source just before this runs (inside the ui
    // pass) with the blurred backdrop for frosted panels; null disables frost.
    impl->backend.SetBackdrop(render::GetVkImageView(view->blur_source),
                              render::GetVkSampler(view->blur_sampler));
    if (impl->draw_data)
      impl->backend.Render(*impl->draw_data, render::GetVkCommandBuffer(cmd));
  };
}

}  // namespace rx

#else  // !RECREATION_HAS_UGUI

namespace rx {

struct GameUi::Impl {};
GameUi::GameUi() = default;
GameUi::~GameUi() = default;
bool GameUi::Initialize(Window&, render::Renderer&) {
  return false;
}
void GameUi::Shutdown() {}
void GameUi::Build(Window&, render::Renderer&, FlyCamera&, f32, render::FrameView*) {}
void GameUi::SetQuest(const HudQuest&) {}
void GameUi::SetChatLines(const base::Vector<base::String>&) {}
void GameUi::SetScoreboard(bool,
                           const base::String&,
                           const base::String&,
                           const base::Vector<base::String>&) {}
void GameUi::SetPrompts(const base::Vector<base::String>&) {}
void GameUi::SetCompassBlips(const base::Vector<CompassBlip>&) {}
void GameUi::SetNametags(const base::Vector<Nametag>&) {}
void GameUi::SetHudGauges(const base::Vector<HudGauge>&) {}
void GameUi::FlashQuestUpdate(const base::String&) {}
void GameUi::SetActivatePrompt(const base::String&) {}
void GameUi::SetHudVisible(bool) {}
void GameUi::SetObjectiveMarker(bool, float, float) {}
void GameUi::SetDialogue(const DialogueView&) {}
void GameUi::SetContainer(const ContainerView&) {}
void GameUi::SetJournal(bool, const base::Vector<HudQuest>&, int) {}
void GameUi::SetWarMap(bool, const base::Vector<WarHoldEntry>&, float) {}
void GameUi::SetEditorView(const EditorView&) {}
void GameUi::SetEditorEventSink(base::Function<void(const EditorUiEvent&)>) {}
void GameUi::ScalePointer(f32 window_x, f32 window_y, f32* canvas_x, f32* canvas_y) const {
  if (canvas_x)
    *canvas_x = window_x;
  if (canvas_y)
    *canvas_y = window_y;
}
void GameUi::SetCharGenView(const CharGenView&) {}
u64 GameUi::CreateUiTexture(int, int, const u8*) {
  return 0;
}
void GameUi::ToggleMenu() {}
bool GameUi::menu_open() const {
  return false;
}
bool GameUi::settings_open() const {
  return false;
}
void GameUi::SetControlsView(const ControlsView&) {}
SettingsRequest GameUi::PollSettingsRequest() {
  return {};
}
bool GameUi::quit_requested() const {
  return false;
}
void GameUi::OpenMainMenu() {}
void GameUi::CloseMainMenu() {}
bool GameUi::main_menu_open() const {
  return false;
}
void GameUi::MainMenuMove(int, int) {}
void GameUi::MainMenuActivate() {}
bool GameUi::MainMenuBack() {
  return false;
}
bool GameUi::MainMenuAtRoot() const {
  return false;
}
void GameUi::SetMainMenuUniverses(const base::Vector<base::String>&, const base::Vector<bool>&) {}
void GameUi::SetMainMenuBackdrop(int, u64) {}
void GameUi::SetMainMenuStats(const MainMenuStats&) {}
void GameUi::SetMainMenuMods(const base::Vector<base::String>&) {}
void GameUi::SetMainMenuNews(const base::Vector<MenuNewsItem>&) {}
void GameUi::SetMainMenuGlyph(const base::String&, u64) {}
int GameUi::selected_universe() const {
  return 0;
}
MainMenuRequest GameUi::PollMainMenuRequest() {
  return {};
}
void GameUi::OpenFirstRun() {}
void GameUi::CloseFirstRun() {}
bool GameUi::first_run_open() const {
  return false;
}
void GameUi::FirstRunNext() {}
void GameUi::FirstRunBack() {}
void GameUi::SetFirstRunView(const FirstRunView&) {}
FirstRunRequest GameUi::PollFirstRunRequest() {
  return {};
}

}  // namespace rx

#endif  // RECREATION_HAS_UGUI
