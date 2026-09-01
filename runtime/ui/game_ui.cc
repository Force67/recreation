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
  // A translated Scaleform screen is authored against the movie's own stage
  // (1280x720 for every Skyrim menu), and Scaleform scales that stage to the
  // viewport. ugui's equivalent is a design size with a contain fit; without it
  // the screen sits at its authored size in a corner. The vanilla screens stand
  // in for the engine's own interface, so switching the whole context is right,
  // and the host's own px scale no longer has anything to scale.
  const ui::VanillaScreen* stage = nullptr;
  if (!VanillaScreens().empty()) {
    const ui::VanillaScreen& first = VanillaScreens()[0];
    if (first.stage_width > 0 && first.stage_height > 0)
      stage = &first;
  }
  if (stage != nullptr) {
    cfg.scale_mode = ugui::ViewportScaleMode::kContain;
    cfg.design_width = stage->stage_width;
    cfg.design_height = stage->stage_height;
    RX_INFO("ui: vanilla stage {}x{}, scaled to the viewport",
            static_cast<int>(stage->stage_width), static_cast<int>(stage->stage_height));
  } else if (UiScale.get() > 1.001f) {
    // A design resolution below the real one makes ugui scale every px dimension
    // by real/design, so scale N means asking for a viewport N times smaller than
    // we actually have.
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

  // The screens name the game's own typeface, so those fonts have to be
  // registered before the tree that asks for them is built.
  if (UsingVanillaUi()) {
    const u32 fonts = ui::LoadVanillaFonts(impl_->ui, ui::VanillaScreenDir(), VanillaScreens());
    if (fonts != 0)
      RX_INFO("ui: {} vanilla font(s)", fonts);
  }

  base::String doc = BuildUi();
  impl_->ui.LoadUiString(doc.c_str(), "hud");
  BindVanillaScreens(impl_->ui, impl_->backend);

  // The shipped menus are empty frames; the game fills them on open, and so
  // does this. See runtime/ui/vanilla_runtime, which runs the movie's own code.
  if (UsingVanillaUi())
    impl_->vanilla_strings = ui::LoadVanillaStrings(ui::VanillaScreenDir());
  impl_->StartVanillaVms();
  if (UsingVanillaUi()) {
    for (const char* fragment : {"topbar", "crosshair", "vitals", "readout", "questtracker"})
      impl_->SetVisible(fragment, false);
  }
  // The legal notice comes up over everything and is the first thing seen.
  impl_->legal_open = bool(ShowLegal);
  impl_->SetVisible("legal", impl_->legal_open);
  // The guided demo's card belongs to the TourDeRecreation gamemode, which
  // drives it entirely through the SDK's widget API. Nothing here touches it
  // again; it is only collapsed once, because markup cannot start hidden and it
  // would otherwise sit over the front screen until the managed world booted.
  impl_->SetVisible("tour_card", false);

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
    for (ui::VanillaRuntime& runtime : impl->vanilla_vms) {
      if (runtime.Click(impl->ui, w.index))
        return;  // the movie's own code took this click
    }
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
      impl->stats_open = false;
      impl->ApplyMenuVisibility();
    } else if (n->name == "btn_settings") {
      impl->settings_open = true;
      impl->stats_open = false;
      impl->ApplyMenuVisibility();
    } else if (n->name == "btn_settings_back") {
      impl->settings_open = false;
      impl->ApplyMenuVisibility();
    } else if (n->name == "btn_stats") {
      impl->stats_open = true;
      impl->settings_open = false;
      impl->stats_page = 0;
      impl->ApplyMenuVisibility();
      impl->ApplyStatsPage();
    } else if (n->name == "btn_stats_prev" || n->name == "btn_stats_next") {
      const size_t pages =
          impl->stats_view.rows.empty()
              ? 1
              : (impl->stats_view.rows.size() + Impl::kStatsRows - 1) / Impl::kStatsRows;
      // Wraps, so the pager works with three widgets and no disabled state.
      impl->stats_page = n->name == "btn_stats_next" ? (impl->stats_page + 1) % pages
                                                     : (impl->stats_page + pages - 1) % pages;
      impl->ApplyStatsPage();
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
  if (UiMenu || UiMenuSettings || UiMenuStats)
    impl_->menu_open = true;
  if (UiMenuSettings)
    impl_->settings_open = true;
  if (UiMenuStats)
    impl_->stats_open = true;
  impl_->ApplyMenuVisibility();  // menu starts hidden unless forced open
  impl_->ApplyStatsPage();
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
  // Esc opens and closes the menu. Walking back out of a sub-panel is the
  // movie's own job, but its `handleInput` reports every key as handled, so
  // routing Esc through it would mean the menu could never be closed at all.
  impl_->menu_open = !impl_->menu_open;
  impl_->settings_open = false;  // always reopen on the main pause screen
  impl_->stats_open = false;
  impl_->ApplyMenuVisibility();
}

bool GameUi::PollReturnToMenu() {
  if (!impl_->initialized || !impl_->return_to_menu)
    return false;
  impl_->return_to_menu = false;
  return true;
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
  if (impl_->initialized && impl_->main_menu_open && !impl_->vanilla_vms.empty()) {
    if (dy != 0) {
      for (ui::VanillaRuntime& runtime : impl_->vanilla_vms)
        runtime.Navigate(impl_->ui, dy < 0 ? "up" : "down");
    }
    return;
  }
  if (!impl_->initialized || !impl_->main_menu_open || impl_->mm_screen != 0)
    return;
  const int count = static_cast<int>(impl_->mm_entries.size());
  if (count == 0)
    return;
  // One flat index across every page: dx walks the row and rolls onto the next,
  // dy drops a grid row, and dy of +-2 is exactly a page (the Q/E step), since
  // the page is always mm_entry / kMenuTiles.
  const int step = dx + dy * kMenuTileCols;
  impl_->mm_entry = base::Clamp(impl_->mm_entry + step, 0, count - 1);
}

void GameUi::MainMenuActivate() {
  if (!impl_->initialized || !impl_->main_menu_open)
    return;
  if (!impl_->vanilla_vms.empty()) {
    // The translated boot menu is the front screen; activating a row is the
    // movie asking the host for what that row means.
    for (ui::VanillaRuntime& runtime : impl_->vanilla_vms)
      runtime.Navigate(impl_->ui, "enter");
    return;
  }
  if (impl_->mm_screen != 0)
    return;
  impl_->LaunchFocusedEntry();
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

void GameUi::SetMainMenuEntries(const base::Vector<MenuEntry>& entries) {
  if (!impl_->initialized)
    return;
  base::Vector<MenuEntry> next = entries;
  // Art is bound separately (SetMainMenuEntryArt) once it has been painted, so a
  // re-push carrying no texture keeps the one already on that tile.
  for (size_t i = 0; i < next.size() && i < impl_->mm_entries.size(); ++i)
    if (!next[i].art && next[i].title == impl_->mm_entries[i].title)
      next[i].art = impl_->mm_entries[i].art;
  impl_->mm_entries = base::move(next);
  impl_->mm_entries_pushed = true;
  impl_->mm_entry =
      base::Clamp(impl_->mm_entry, 0, base::Max(0, static_cast<int>(entries.size()) - 1));
}

void GameUi::SetMainMenuEntryArt(int entry, u64 texture) {
  if (!impl_->initialized || entry < 0 || entry >= static_cast<int>(impl_->mm_entries.size()))
    return;
  impl_->mm_entries[entry].art = texture;
}

int GameUi::selected_entry() const {
  if (!impl_->initialized || impl_->mm_entries.empty())
    return -1;
  return impl_->mm_entry;
}

void GameUi::SetMainMenuUniverses(const base::Vector<base::String>& names,
                                  const base::Vector<bool>& available) {
  if (!impl_->initialized)
    return;
  if (!names.empty())
    impl_->mm_universe_names = names;
  if (!available.empty())
    impl_->mm_available = available;
  impl_->RebuildDerivedEntries();
}

void GameUi::SetMainMenuBackdrop(int universe, u64 texture) {
  if (!impl_->initialized || universe < 0 || universe >= kMenuUniverses)
    return;
  impl_->mm_backdrop[universe] = texture;
  impl_->RebuildDerivedEntries();
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
  if (!impl_->initialized)
    return;
  // The journal's bottom bar is the game's RequestPlayerInfo answer; recreation
  // has the clock, so the date is real. There is no XP source yet, so the level
  // meter sits where the engine leaves it.
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
  if (!impl_->initialized || impl_->mm_entry >= static_cast<int>(impl_->mm_entries.size()))
    return 0;
  return impl_->mm_entries[impl_->mm_entry].universe;
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

void GameUi::SetMainMenuTour(const base::String& title, bool available) {
  if (!impl_->initialized)
    return;
  impl_->mm_tour_title = title;
  impl_->mm_tour_available = available;
}

void GameUi::OpenLoading(const base::String& title) {
  if (!impl_->initialized)
    return;
  impl_->loading_open = true;
  impl_->loading = LoadingView{};
  impl_->loading.title = title;
  // A negative last-tip time makes the first Apply pick a tip immediately, so
  // the screen is never up with an empty hint line.
  impl_->ld_tip_from = -kLoadingTipSeconds;
  impl_->ApplyLoading();
}

void GameUi::CloseLoading() {
  if (!impl_->initialized)
    return;
  impl_->loading_open = false;
  impl_->ApplyLoading();
}

bool GameUi::loading_open() const {
  return impl_->initialized && impl_->loading_open;
}

void GameUi::SetLoadingView(const LoadingView& view) {
  if (impl_->initialized)
    impl_->loading = view;
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

bool GameUi::stats_open() const {
  return impl_->initialized && impl_->stats_open;
}

void GameUi::SetStatsView(const StatsView& view) {
  if (!impl_->initialized)
    return;
  impl_->stats_view = view;
  impl_->ApplyStatsPage();
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

void GameUi::SetPlayerMap(const PlayerMapView& view) {
  if (impl_->initialized)
    impl_->player_map = view;
}

void GameUi::UpdateUiTexture(u64 texture, const u8* rgba) {
  if (!impl_->initialized || texture == 0 || !rgba)
    return;
  impl_->backend.UpdateTexture(texture, rgba);
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

  // The front screens (legal card, setup wizard, NEXUS menu) are authored
  // against a fixed 1920x1080 stage and fitted to the viewport, the way a
  // Scaleform movie is. Without it a 1440p or 4K display draws that composition
  // at its authored size in the top-left corner with the rest of the screen
  // empty. In-world UI keeps the raw backbuffer space it measures against.
  const bool frontend = impl->legal_open || impl->first_run_open || impl->main_menu_open ||
                        impl->loading_open;
  impl->ui.set_ui_scale(frontend ? base::Min(impl->host.window_width / kFrontendDesignWidth,
                                             impl->host.window_height / kFrontendDesignHeight)
                                 : 0.0f);

  // Feed the per-frame input snapshot into ultragui's queue, scaling the cursor
  // from window space into the (possibly larger) backbuffer canvas so clicks
  // line up with the widgets.
  const InputState& in = window.input();
  impl->ui_time += frame_delta;
  for (ui::VanillaRuntime& runtime : impl->vanilla_vms) {
    runtime.Tick(impl->ui, frame_delta);
  }
  if (const float at = PauseAt.get(); at > 0.0f) {
    const auto crossed = [&](float t) {
      return impl->ui_time >= t && impl->ui_time - frame_delta < t;
    };
    if (crossed(at))
      ToggleMenu();
    if (const int pick = PausePick.get(); pick >= 0 && crossed(at + 1.0f)) {
      for (ui::VanillaRuntime& runtime : impl->vanilla_vms) {
        for (int i = 0; i < pick; ++i)
          runtime.Navigate(impl->ui, "down");
        runtime.Navigate(impl->ui, "enter");
      }
    }
    // A second beat picks the first entry of whatever sub-panel that opened,
    // which is how the quit list's "Main Menu" gets exercised.
    if (PausePick.get() >= 0 && crossed(at + 2.0f)) {
      for (ui::VanillaRuntime& runtime : impl->vanilla_vms)
        runtime.Navigate(impl->ui, "enter");
    }
  }

  // The legal notice: counts itself down, and any key, pad button or click takes
  // it away early. It swallows that input so the press does not also land on
  // whatever it is covering: the guards below read the state the frame STARTED
  // in, because the dismissing press is still set in `in.pressed` after this
  // clears the flag, and would otherwise activate a row of the menu underneath.
  const bool legal_was_open = impl->legal_open;
  if (impl->legal_open) {
    impl->legal_left -= frame_delta;
    const int remaining = static_cast<int>(std::ceil(base::Max(0.0f, impl->legal_left)));
    if (remaining != impl->legal_shown) {
      impl->legal_shown = remaining;
      ugui::SetText(impl->ui.FindWidget("legal_count"), base::ToString(remaining).c_str());
    }
    bool dismiss = impl->legal_left <= 0.0f;
    for (int k = 0; !dismiss && k < static_cast<int>(Key::kCount); ++k)
      dismiss = in.pressed[k];
    for (int b = 0; !dismiss && b < static_cast<int>(MouseButton::kCount); ++b)
      dismiss = in.mouse[b] && !impl->prev_mouse[b];
    if (!dismiss && window.gamepad().connected) {
      for (int b = 0; !dismiss && b < static_cast<int>(GamepadButton::kCount); ++b)
        dismiss = window.gamepad().pressed[b];
    }
    if (dismiss) {
      impl->legal_open = false;
      impl->SetVisible("legal", false);
      RX_INFO("ui: legal notice dismissed");
    }
  }
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

  // Test hook: RX_UI_CLICK="fr_begin,fr_chk0,..." clicks each named widget in
  // turn, one every RX_UI_CLICK_STRIDE frames (default 12), by pushing a real
  // press and release at the widget's centre. That is the mouse's own path -
  // ugui hit-tests the deepest child and the routers walk back up - so it
  // exercises the click routing rather than calling a handler directly, and it
  // is what drives the onboarding flow in a headless capture.
  if (const char* script = UiClick.get(); script != nullptr && *script) {
    if (impl->click_script.empty()) {
      base::String name;
      for (const char* c = script;; ++c) {
        if (*c == ',' || *c == '\0') {
          if (!name.empty())
            impl->click_script.push_back(name);
          name.clear();
          if (*c == '\0')
            break;
        } else {
          name += *c;
        }
      }
    }
    const int stride = base::Max(1, UiClickStride.get());
    if (++impl->click_frame >= stride) {
      impl->click_frame = 0;
      if (impl->click_index < static_cast<int>(impl->click_script.size())) {
        const base::String& name = impl->click_script[impl->click_index++];
        const ugui::wid w = impl->ui.FindWidget(name.c_str());
        const ugui::Transform* t =
            w.valid() ? impl->ui.world().Get<ugui::Transform>(w) : nullptr;
        if (t == nullptr) {
          RX_WARN("ui click: no widget '{}'", name);
        } else {
          const ugui::Vec2 at{t->rect.x + t->rect.w * 0.5f, t->rect.y + t->rect.h * 0.5f};
          q.PushMove(at);
          q.PushButton(ugui::MouseButton::kLeft, true);
          q.PushButton(ugui::MouseButton::kLeft, false);
          RX_INFO("ui click: {} at {:.0f},{:.0f}", name, at.x, at.y);
        }
      }
    }
  }

  // Feed gamepad + keyboard navigation into ugui's focus ring so menus with
  // tab-index'd widgets (pause / settings) are navigable by pad and keyboard.
  // ugui drives nav/activation internally from these queued events.
  const GamepadState& pad = window.gamepad();
  bool pad_pressed[static_cast<int>(GamepadButton::kCount)] = {};
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
      pad_pressed[b] = down;
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
  if (!legal_was_open) {
    if (in.key_pressed(Key::kTab))
      q.PushKey(258, 0, true, false, shift_mod);
    if (in.key_pressed(Key::kReturn))
      q.PushKey(257, 0, true, false, 0);
  }

  // The vanilla menus are lists the game drives itself rather than focus rings,
  // so they take up/down and activate directly instead of going through ugui's
  // navigation. Their rows are all one widget deep and carry no focus index.
  // The interpreter path takes navigation through the movies' own components,
  // which reaches whichever screen is up without the host knowing which.
  if (!legal_was_open && ui::VanillaRuntime::Enabled()) {
    const char* navigation = nullptr;
    if (in.key_pressed(Key::kArrowUp) || pad_pressed[static_cast<int>(GamepadButton::kDpadUp)])
      navigation = "up";
    else if (in.key_pressed(Key::kArrowDown) ||
             pad_pressed[static_cast<int>(GamepadButton::kDpadDown)])
      navigation = "down";
    else if (in.key_pressed(Key::kReturn) || pad_pressed[static_cast<int>(GamepadButton::kSouth)])
      navigation = "enter";
    if (navigation != nullptr) {
      for (ui::VanillaRuntime& runtime : impl->vanilla_vms)
        runtime.Navigate(impl->ui, navigation);
      // "Press any button to start": a Fallout 4 screen waits on its splash
      // until the game moves it on, which is the game's decision and not one
      // the movie makes for itself. Any of the keys above is that button.
      const base::Vector<swf::As3Value> none;
      for (ui::VanillaRuntime& runtime : impl->vanilla_vms)
        runtime.CallAs3(impl->ui, "ReturnToMainState", none);
    }
  }

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
  // The only HUD fragment driven every frame rather than on an edge, so it is
  // also the only one that needs the vanilla gate here: hiding it once at
  // startup would not survive the next quest update.
  const bool has_quest = !impl->quest.title.empty();
  impl->SetVisible("questtracker", has_quest && !UsingVanillaUi());
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

  // World map: the painted canvas plus the discovered-location rail beside it.
  // The picture itself is a texture the engine repaints, so there is nothing to
  // do here but bind it and fill the list.
  impl->SetVisible("pmap_box", impl->player_map.open);
  if (impl->player_map.open) {
    const GameUi::PlayerMapView& map = impl->player_map;
    impl->SetText("pmap_head", map.title.empty() ? base::String("Map") : map.title);
    impl->SetText("pmap_sub", map.subtitle);
    impl->SetText("pmap_where", map.where);
    impl->SetText("pmap_status", map.status);
    if (map.canvas != 0)
      ugui::SetImageTexture(impl->Need("pmap_canvas"), map.canvas, 1.0f, 1.0f);
    impl->SetVisible("pmap_canvas", map.canvas != 0);
    for (int i = 0; i < kPlayerMapRows; ++i) {
      const char* row = impl->Pooled("pmap_row", i);
      if (i >= static_cast<int>(map.rows.size())) {
        impl->SetVisible(row, false);
        continue;
      }
      const GameUi::PlayerMapView::Row& entry = map.rows[i];
      const bool on = i == map.selected;
      impl->SetText(row, (on ? base::String("> ") : base::String("  ")) + entry.name + "   " +
                             entry.detail);
      // Three values, no colour: selected is white, a place that cannot be
      // travelled to is muted, the rest sit between.
      impl->SetTextColor(row, Rgba(on ? 0xffffffffu
                                      : (entry.travelable ? 0x9a9a9affu : 0x5e5e5effu)));
      impl->SetVisible(row, true);
    }
  }

  // Map editor overlay (asset browser, toolbar, inspector, status, reticle).
  impl->ApplyEditorView();

  // Character-creation overlay (race/sex/preset docks + slider list).
  impl->ApplyCharGenView();

  // NEXUS main menu (the startup front screen, on top of everything).
  impl->ApplyMainMenu();

  // First-run setup wizard (the out-of-box experience, above even the menu).
  impl->ApplyFirstRun();

  // The loading screen, which covers all of it while a universe comes online.
  impl->ApplyLoading();

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
void GameUi::SetPlayerMap(const PlayerMapView&) {}
void GameUi::UpdateUiTexture(u64, const u8*) {}
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
bool GameUi::PollReturnToMenu() {
  return false;
}
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
bool GameUi::stats_open() const {
  return false;
}
void GameUi::SetStatsView(const StatsView&) {}
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
void GameUi::SetMainMenuEntries(const base::Vector<MenuEntry>&) {}
void GameUi::SetMainMenuEntryArt(int, u64) {}
int GameUi::selected_entry() const {
  return -1;
}
void GameUi::SetMainMenuUniverses(const base::Vector<base::String>&, const base::Vector<bool>&) {}
void GameUi::SetMainMenuTour(const base::String&, bool) {}
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
void GameUi::OpenLoading(const base::String&) {}
void GameUi::CloseLoading() {}
bool GameUi::loading_open() const {
  return false;
}
void GameUi::SetLoadingView(const LoadingView&) {}

}  // namespace rx

#endif  // RECREATION_HAS_UGUI
