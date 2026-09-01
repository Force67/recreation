#ifndef RUNTIME_UI_GAME_UI_INTERNAL_H
#define RUNTIME_UI_GAME_UI_INTERNAL_H

// Shared innards of GameUi, split across game_ui*.cc.
//
// GameUi::Impl lives here because the editor, the menus and the document
// builder all drive it. Everything here was file-local before the split and is
// `inline` only so several .cc files may include it; none of it is an interface.

#include <base/algorithm.h>
#include <base/containers/pair.h>
#include <base/containers/vector.h>
#include <base/functional/function.h>
#include <base/memory/move.h>
#include <base/memory/unique_pointer.h>
#include <base/option.h>
#include <base/strings/to_string.h>
#include <base/strings/xstring.h>

#include "runtime/ui/game_ui.h"

#include "runtime/camera/fly_camera.h"
#include "runtime/ui/touch_pointer.h"

#if defined(RECREATION_HAS_UGUI)

#include <ugui/core/color.h>
#include <ugui/style/style.h>
#include <ugui/ultragui.h>
#include <ugui/widgets/image.h>
#include <ugui/widgets/text.h>
#include <ugui/widgets/widget.h>
#include <ugui/widgets/widget_registry.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/log.h"
#include "core/window.h"
#include "render/core/renderer.h"
#include "render/rhi/vulkan_interop.h"
#include "runtime/ui/gui_backend.h"
#include "runtime/ui/ugui_platform.h"
#include "runtime/ui/vanilla_list.h"
#include "runtime/ui/vanilla_runtime.h"
#include "runtime/ui/vanilla_ui.h"

namespace rx {

namespace fs = std::filesystem;

// Config options, populated from the environment once at startup (see
// base::InitOptionsFromEnv). Namespace scope so they register before that runs
// and stay visible to every use below. UiDirOpt avoids colliding with UiDir().
inline base::Option<const char*> UiDirOpt{"ui.dir", nullptr, "RECREATION_UI_DIR"};
inline base::Option<const char*> UiFont{"ui.font", nullptr, "RECREATION_UI_FONT"};
inline base::Option<const char*> UiFontMono{"ui.font.mono", nullptr, "RECREATION_UI_FONT_MONO"};
inline base::Option<const char*> UiFontBold{"ui.font.bold", nullptr, "RECREATION_UI_FONT_BOLD"};
inline base::Option<bool> UiHotReload{"ui.hot.reload", false, "RECREATION_UI_HOT_RELOAD"};
inline base::Option<bool> UiMenu{"ui.menu", false, "RECREATION_UI_MENU"};
// Opens the pause menu straight into its controls sub-view. The rebind list is
// otherwise only reachable by clicking Settings, which leaves it out of reach
// of a scripted RX_SCREENSHOT capture.
inline base::Option<bool> UiMenuSettings{"ui.menu.settings", false, "RECREATION_UI_MENU_SETTINGS"};
// Same, for the stats sub-view, which is otherwise only reachable by clicking.
inline base::Option<bool> UiMenuStats{"ui.menu.stats", false, "RECREATION_UI_MENU_STATS"};
// Global UI scale. The .ugui screens are authored for a desktop pointer, where
// a 50px button is comfortable; on the Deck's 216 PPI panel that is 5.9mm
// against a fingertip contact patch of 8-10mm. ugui scales every px dimension
// (font size, padding, fixed dimensions) when handed a design resolution below
// the real one, so one knob grows the whole interface rather than restyling
// every screen. 1.0 leaves the desktop layout untouched.
inline base::Option<float> UiScale{"ui.scale", 1.0f, "RECREATION_UI_SCALE"};
inline base::Option<bool> MainMenu{"main.menu", false, "RECREATION_MAIN_MENU"};
inline base::Option<bool> FirstRun{"first.run", false, "RECREATION_FIRST_RUN"};
// The startup legal notice. On by default and meant to stay that way; RX_LEGAL=0
// exists so the screenshot harnesses (which capture a frame a second or two in)
// do not all photograph this card instead of the screen under test.
inline base::Option<bool> ShowLegal{"legal", true, "RX_LEGAL"};
// Test hook, matching RX_MENU_AUTOPLAY on the front menu: opens the pause menu
// this many seconds in, so the journal path can be captured without a keypress.
inline base::Option<float> PauseAt{"ui.pause_at", 0.0f, "RX_PAUSE_AT"};
// With RX_PAUSE_AT, walk this many rows down the category list a second later
// and pick that entry, so a sub-panel can be captured the same way.
inline base::Option<int> PausePick{"ui.pause_pick", -1, "RX_PAUSE_PICK"};
constexpr f32 kLegalSeconds = 5.0f;
// The stage the front screens (legal notice, setup wizard, NEXUS menu) are
// authored against. ugui's design space here is the raw backbuffer, so they are
// fitted to the viewport while one of them is up; left alone, a 1920-wide
// composition draws at its authored size in the corner of a larger screen.
constexpr f32 kFrontendDesignWidth = 1920.0f;
constexpr f32 kFrontendDesignHeight = 1080.0f;
inline base::Option<const char*> FirstRunStep{"first.run.step", nullptr, "RECREATION_FIRST_RUN_STEP"};
// Test hook: a comma-separated list of widget names to click in order, one
// every UiClickStride frames. Drives the front-end flows headlessly.
inline base::Option<const char*> UiClick{"ui.click", nullptr, "RX_UI_CLICK"};
inline base::Option<int> UiClickStride{"ui.click.stride", 12, "RX_UI_CLICK_STRIDE"};
// RX_UI_KEY="down,down,enter": drives the focus ring rather than aiming at a
// widget by name, which is the only way to exercise keyboard navigation.
// Shares RX_UI_CLICK_STRIDE.
inline base::Option<const char*> UiKey{"ui.key", nullptr, "RX_UI_KEY"};
// Arrow keys move focus between the interactive widgets of whatever ugui screen
// is up, and Enter activates. RX_UI_KEYBOARD_NAV=0 hands the arrows back.
inline base::Option<bool> UiKeyboardNav{"ui.keyboard.nav", true, "RX_UI_KEYBOARD_NAV"};

// Scrolling compass geometry. 8 marks per 360deg turn, 3 turns so the strip
// always covers the window whatever the heading; the engine slides it by
// setting compass_strip's left offset each frame.
constexpr float kCompassWindow = 380.0f;
constexpr float kCompassLabel = 75.0f;
constexpr int kCompassTurns = 3;
constexpr float kCompassCenter = kCompassWindow * 0.5f;

inline const char* const kCardinals[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};

inline float CompassStripLeft(float heading_deg) {
  float eff = heading_deg + 360.0f;  // middle turn
  return kCompassCenter - (eff / 45.0f) * kCompassLabel - kCompassLabel * 0.5f;
}

// The tracked quest never shows more than a handful of objectives at once, so a
// fixed pool of pre-declared rows is filled and toggled each frame; the static
// ultragui document has no way to add widgets on the fly.
constexpr int kQuestObjectiveRows = 6;
constexpr int kDialogueOptionRows = 4;  // matches the 1-4 selection keys
constexpr int kJournalRows = 6;         // quests listed in the journal (1-N pick, 1-4 usable)
constexpr int kJournalObjRows = 6;      // objectives shown for the selected journal quest
constexpr int kWarHoldRows = 9;         // Skyrim's nine holds on the war-map panel
constexpr int kPlayerMapRows = GameUi::PlayerMapView::kRows;  // location rows beside the map
constexpr int kContainerRows = 14;      // item rows in the container loot panel
constexpr int kHudGaugeRows = 6;        // pooled managed-gameplay gauge bars (oxygen, rads, ...)
constexpr int kChatRows = 8;            // visible lines in the multiplayer chat box
constexpr int kScoreRows = 12;          // player rows in the multiplayer scoreboard
constexpr int kPromptRows = 3;          // stacked multiplayer interaction prompts
constexpr int kNametags = 16;           // floating world-space player nametags
constexpr float kToastSeconds = 4.0f;
constexpr float kLocationRevealSeconds = 3.5f;

// Declared here rather than beside the editor palette below: the domain accent
// and GameUi::Impl both need it.
inline ugui::Color Rgba(u32 hex) {
  return ugui::Color::FromRgba8((hex >> 24) & 0xff, (hex >> 16) & 0xff, (hex >> 8) & 0xff,
                                hex & 0xff);
}

// Domain accent: one colour per world, replacing white only in the live or
// selected role. Skyrim stays white and doubles as the front-end neutral.
// See docs/ui-mock/untarnished_domains.html.
constexpr u32 kAccentNeutral = 0xffffffffu;  // Skyrim, and no-world-loaded
constexpr u32 kAccentFallout = 0x2fe87affu;  // phosphor
constexpr u32 kAccentStarfield = 0xff9d47ffu;  // scanner amber
constexpr u32 kAccentOblivion = 0xd8b26affu;  // leaf gold
constexpr u32 kAccentMorrowind = 0x9ecfd8ffu;  // ash glass

// Widgets carrying the accent as a fill. Anything absent stays monochrome.
inline const char* const kAccentFills[] = {
    "ch_dot",      // crosshair centre: the accent at rest, in-world
    "quest_rule",  // the rule over the tracked objectives
};

// Widgets carrying it as a border instead.
inline const char* const kAccentBorders[] = {
    "acc_prompt_key",  // the interaction prompt's keycap
};

// Substring match: the engine names domains variously ("Skyrim Special
// Edition", "Fallout: New Vegas") and the family selects the colour.
inline u32 AccentForUniverse(const base::String& name) {
  if (name.find("Fallout") != base::String::npos)
    return kAccentFallout;
  if (name.find("Starfield") != base::String::npos)
    return kAccentStarfield;
  if (name.find("Oblivion") != base::String::npos)
    return kAccentOblivion;
  if (name.find("Morrowind") != base::String::npos)
    return kAccentMorrowind;
  return kAccentNeutral;  // Skyrim, and anything unrecognised
}

// NEXUS main menu.
constexpr int kMenuUniverses = 3;    // Skyrim, Fallout 4, Starfield
constexpr int kMenuTileCols = 4;     // tiles across one grid row
constexpr int kMenuTiles = 8;        // pooled tiles: two rows, and one page
constexpr int kMenuSpineTicks = 10;  // pooled load-order ticks per tile
constexpr int kMenuPips = 8;         // pooled page pips
constexpr int kMenuModRows = 16;     // pooled rows on the Mods sub-screen
constexpr int kFirstRunSteps = 5;  // welcome, locate, preferences, mods, ready
constexpr f32 kFirstRunPageFade = 0.16f;  // seconds a page turn takes to settle
constexpr int kFirstRunGames = 3;  // game rows on the locate page
constexpr int kLoadingPhases = 6;         // rows on the loading screen's phase rail
constexpr f32 kLoadingTipSeconds = 6.0f;  // how long each "while you wait" tip holds

// Editor icon glyphs, composed from panels: the UI font (Noto Sans) has no
// symbol set, so toolbar / tree / inspector icons are drawn as little stacks of
// filled and outlined rectangles inside an 18x18 relative box, the same trick

// The editor/chargen palette, on the same monochrome system as the rest of the
// UI: three text values, white for the live element, and the one signal red
// reserved for destructive actions. Object-type icons are told apart by value.
inline const ugui::Color kEdAccent = Rgba(0xffffffff);      // indigo accent
inline const ugui::Color kEdAccentSoft = Rgba(0xffffff1f);  // selected-row wash
inline const ugui::Color kEdClear = Rgba(0x00000000);
inline const ugui::Color kEdTxP = Rgba(0xffffffff);  // primary text
inline const ugui::Color kEdTxS = Rgba(0x9a9a9aff);  // secondary text
inline const ugui::Color kEdTxM = Rgba(0x5e5e5eff);  // muted text
inline const ugui::Color kEdLeaf = Rgba(0xffffffff);
inline const ugui::Color kEdField = Rgba(0x000000ff);
inline const ugui::Color kEdEyeOn = Rgba(0xffffffff);
inline const ugui::Color kEdEyeOff = Rgba(0x5e5e5eff);
inline const ugui::Color kEdToggleOff = Rgba(0x3a3a3aff);
inline const ugui::Color kEdIcoGroup = Rgba(0x5e5e5eff);
inline const ugui::Color kEdIcoMesh = Rgba(0xffffffff);
inline const ugui::Color kEdIcoLight = Rgba(0x9a9a9aff);
inline const ugui::Color kEdCardBorder = Rgba(0xffffff14);
inline const ugui::Color kEdCat = Rgba(0x9a9a9aff);

// Built in game_ui_editor.cc / game_ui_menu.cc, assembled by BuildUi() in
// game_ui_doc.cc.
base::String BuildEditorSection();
base::String BuildCharGenSection();
base::String BuildTopbarSection();
base::String BuildUi();
base::String LoadUiFragment(const char* name);
fs::path UiDir();
constexpr int kUiFragmentCount = 22;
extern const char* const kUiFragments[kUiFragmentCount];

// Screens translated from the games' own Scaleform movies by tools/swfdump
// (see RX_VANILLA_UI), and the queries the rest of GameUi makes about them.
// Defined in game_ui_doc.cc beside the rest of the document assembly.
base::Vector<ui::VanillaScreen>& VanillaScreens();

// Which state of the host's UI a translated screen stands in for. The movies
// are separate files with no notion of when they are up: the game decides, and
// so does this. A screen with no role is chrome that belongs to gameplay (the
// HUD, the cursor, the fader), which is what most of them are.
enum class VanillaRole : u8 { kHud, kFrontMenu, kPauseMenu };

VanillaRole VanillaRoleOf(base::StringRef name);

// The root panel the exporter emits for a screen. Namespaced, because a movie's
// stem collides with the host's own fragments (Fallout 4 ships mainmenu.swf).
base::String VanillaRootName(base::StringRef screen);

// True once any translated Scaleform screen is loaded. Asking for the game's own
// interface means asking for it instead of recreation's, so the engine's HUD
// fragments stay hidden rather than drawing a second one over the top.
bool UsingVanillaUi();

// The vanilla screens reference their art by widget name; the textures have to
// be re-bound every time the tree is rebuilt.
void BindVanillaScreens(ugui::UIContext& context, ugui::TextureBackend& backend);
// Font discovery, in game_ui_doc.cc.
const char* FindFont();
const char* FindMonoFont();
const char* FindBoldFont();

struct GameUi::Impl {
  ugui::UIContext ui;
  ui::UguiHostState host;
  ui::GuiRenderBackend backend;
  ugui::FontHandle font = ugui::kInvalidFont;
  u32 font_revision = ~0u;
  const ugui::DrawData* draw_data = nullptr;
  bool initialized = false;
  bool menu_open = false;
  bool settings_open = false;  // settings sub-view of the pause menu
  bool stats_open = false;     // stats sub-view of the pause menu
  bool quit_requested = false;
  bool return_to_menu = false;  // pause menu asked to leave the world
  f32 ui_time = 0;              // seconds since the UI came up, for the test hooks

  // The startup legal notice: a full-bleed card over everything, dismissed by
  // any input or by its own countdown running out.
  bool legal_open = false;
  f32 legal_left = kLegalSeconds;
  int legal_shown = -1;  // last count written, so the text is not set every frame

  // The screens' own ActionScript, running against the translated widgets. One
  // per loaded vanilla screen; the movies drive their own menus.
  base::Vector<ui::VanillaRuntime> vanilla_vms;
  base::UnorderedMap<base::String, base::String> vanilla_strings;
  SettingsRequest settings_request;  // raised by the settings panel, polled by the engine
  // Rows the stats pane shows at once; the markup authors exactly this many.
  static constexpr size_t kStatsRows = 18;
  StatsView stats_view;
  size_t stats_page = 0;
  bool prev_mouse[3] = {};
  TouchPointerState touch_pointer;
  // RX_UI_CLICK playback: the parsed name list and where it has got to.
  base::Vector<base::String> click_script;
  int click_index = 0;
  int click_frame = 0;
  // RX_UI_KEY playback, the keyboard-navigation counterpart of the above.
  base::Vector<base::String> key_script;
  int key_index = 0;
  int key_frame = 0;
  float pointer_scale_x = 1.0f;
  float pointer_scale_y = 1.0f;
  bool prev_pad[static_cast<int>(GamepadButton::kCount)] = {};  // gamepad edge tracking
  float stamina = 1.0f;
  int last_fps = 0;  // last computed fps, shown in the editor status bar

  // Hot reload of the .ugui fragments (RECREATION_UI_HOT_RELOAD). Watches each
  // fragment's mtime and rebuilds the tree when one changes.
  bool hot_reload = false;
  float reload_timer = 0.0f;  // throttle the mtime poll
  base::Vector<fs::file_time_type> fragment_mtimes;

  // Quest HUD state, set by the engine and applied each frame.
  HudQuest quest;
  base::Vector<HudGauge> hud_gauges;      // managed gameplay bars (oxygen, rads, ...)
  base::Vector<base::String> chat_lines;  // multiplayer chat box, newest last
  bool scoreboard_open = false;           // multiplayer scoreboard (hold-Tab list)
  base::String scoreboard_title;
  base::String scoreboard_header;
  base::Vector<base::String> scoreboard_rows;
  base::Vector<base::String> mp_prompts;            // multiplayer interaction prompts
  // Map blips are still accepted by SetCompassBlips, but the compass carries
  // letters only now, so nothing draws them. Kept so the callers keep building.
  base::Vector<GameUi::CompassBlip> compass_blips;
  base::Vector<GameUi::Nametag> nametags;           // floating world-space labels
  base::String toast_text;
  float toast_age = kToastSeconds + 1.0f;  // starts expired, so hidden
  // Area-title reveal: the last location announced, and how long ago.
  base::String loc_shown;
  float loc_reveal_age = kLocationRevealSeconds + 1.0f;  // starts expired
  base::String activate_prompt;
  DialogueView dialogue;
  ContainerView container;
  // Objective compass waypoint, driven by the engine each frame.
  bool marker_active = false;
  float marker_bearing = 0.0f;   // degrees, 0 = ahead, + = right
  float marker_distance = 0.0f;  // meters
  // Quest journal overlay, driven by the engine.
  bool journal_open = false;
  base::Vector<HudQuest> journal;
  int journal_selected = -1;

  // World-map overlay, painted and driven by player_map.cc.
  GameUi::PlayerMapView player_map;

  // War-map overlay, driven by the managed Civil War campaign.
  bool war_map_open = false;
  base::Vector<GameUi::WarHoldEntry> war_holds;
  float war_progress = 0.0f;

  // Map editor overlay state and the sink that receives its widget clicks.
  EditorView editor;
  base::Function<void(const EditorUiEvent&)> editor_sink;
  bool editor_prev_active = false;  // edge-detect to hide/restore the gameplay HUD

  // Character-creation overlay state (a pure view; CharGen hit-tests its own
  // panels, so there is no click sink). Edge-detect hides the gameplay HUD.
  CharGenView chargen;
  bool chargen_prev_active = false;
  void ApplyCharGenView();

  // NEXUS main menu state, driven by the engine and the click router. The
  // request is raised here (click / keyboard) and consumed by PollMainMenuRequest.
  bool main_menu_open = false;
  int mm_screen = 0;   // 0 root, 1 multiplayer, 2 mods, 3 settings, 4 profile
  int mm_mp_mode = 0;  // last multiplayer choice: 0 host, 1 join
  MainMenuRequest mm_request;
  MainMenuStats mm_stats;
  // The tile grid, in display order, and the focused tile within it. The page is
  // not stored: it is mm_entry / kMenuTiles, so focus and paging cannot disagree.
  base::Vector<GameUi::MenuEntry> mm_entries;
  bool mm_entries_pushed = false;  // the engine replaced the derived set below
  int mm_entry = 0;
  base::Vector<base::String> mm_universe_names{"Skyrim", "Fallout 4", "Starfield"};
  base::Vector<bool> mm_available{true, true, true};
  base::String mm_tour_title;        // guided demo in the top nav; empty = none staged
  bool mm_tour_available = false;    // its world is located, so it can be launched
  base::Vector<base::String> mm_mods;
  base::Vector<MenuNewsItem> mm_news;
  u64 mm_backdrop[kMenuUniverses] = {0, 0, 0};
  base::Vector<base::Pair<base::String, u64>> mm_glyphs;  // emblem widget -> texture
  bool mm_prev_open = false;  // edge-detect to hide the gameplay HUD while open

  int mm_page() const { return mm_entry / kMenuTiles; }
  int mm_pages() const {
    return base::Max(1, (static_cast<int>(mm_entries.size()) + kMenuTiles - 1) / kMenuTiles);
  }

  // First-run setup wizard state. The wizard owns its page (fr_step) and its
  // interactive selections (dropdowns, toggles); the engine pushes the located
  // games / mods dir into fr_view and consumes the request raised below.
  bool first_run_open = false;
  int fr_step = 0;                        // 0 welcome .. 4 ready
  int fr_anim_step = -1;                  // page the turn animation is playing for
  f32 fr_anim_from = 0.0f;                // ui_time that turn started
  int fr_mode = 0;                        // default-mode selection
  int fr_diff = 1;                        // difficulty dropdown selection
  bool fr_check[3] = {true, true, true};  // enable mods / diagnostics / updates
  FirstRunView fr_view;
  FirstRunRequest fr_request;

  // Loading screen state. The engine owns everything shown here and pushes it
  // through SetLoadingView; the screen only rotates its own tip.
  bool loading_open = false;
  LoadingView loading;
  int ld_tip = 0;             // which "while you wait" tip is up
  f32 ld_tip_from = -1000.0f;  // ui_time the current tip came up

  // Drives every main-menu widget from the state above each frame; collapses the
  // whole overlay when closed. Launch boots the focused tile, if it is playable.
  void ApplyMainMenu();
  void LaunchFocusedEntry();
  // Stands a tile grid up from the three located universes, so the menu works
  // before anything calls SetMainMenuEntries. A pushed grid wins.
  void RebuildDerivedEntries();
  // Climbs from a clicked widget to the nearest menu-handled name and acts on it.
  // Returns true if it consumed the click.
  bool RouteMainMenuClick(ugui::wid target);

  // First-run wizard: drive every widget from the state above; route a click to
  // the page it belongs to; advance/retreat the page (the keyboard helpers and
  // the primary button share AdvanceFirstRun). fr_located() counts found games.
  void ApplyFirstRun();
  bool RouteFirstRunClick(ugui::wid target);

  // Loading screen: drive the phase rail, the progress readout and the rotating
  // key tip from the view the engine pushed.
  void ApplyLoading();
  void AdvanceFirstRun();
  void RetreatFirstRun();
  int fr_located() const {
    int n = 0;
    for (const auto& g : fr_view.games)
      if (g.located)
        ++n;
    return n;
  }

  // FindWidget returns an invalid handle for an unknown name and every Set*
  // below then quietly does nothing, so renaming a widget in a .ugui breaks it
  // silently. Need() reports the miss once instead.
  ugui::wid Need(const char* name) {
    ugui::wid w = ui.FindWidget(name);
    if (!w.valid()) {
      for (const auto& seen : missing_names)
        if (seen == name)
          return w;
      missing_names.push_back(name);
      RX_WARN("ui: no widget named '{}' (renamed or removed in a .ugui?)", name);
    }
    return w;
  }

  // Pooled rows are named "<prefix><index>", built once instead of
  // concatenated every frame.
  const char* Pooled(const char* prefix, int i) {
    int slot = -1;
    for (int k = 0; k < static_cast<int>(pool_prefix.size()); ++k)
      if (std::strcmp(pool_prefix[k], prefix) == 0) {
        slot = k;
        break;
      }
    if (slot < 0) {
      pool_prefix.push_back(prefix);
      pool_names.push_back({});
      slot = static_cast<int>(pool_prefix.size()) - 1;
    }
    base::Vector<base::String>& names = pool_names[slot];
    while (static_cast<int>(names.size()) <= i)
      names.push_back(base::String(prefix) + base::ToString(static_cast<int>(names.size())));
    return names[i].c_str();
  }

  void SetText(const char* name, const char* text) {
    ugui::SetText(Need(name), text);
  }
  void SetText(const char* name, const base::String& text) { SetText(name, text.c_str()); }

  base::Vector<base::String> missing_names;   // reported once each
  base::Vector<const char*> pool_prefix;
  base::Vector<base::Vector<base::String>> pool_names;

  void SetStyleField(const char* name, void (*mutate)(ugui::Style&, float), float arg) {
    ugui::wid w = Need(name);
    if (!w.valid())
      return;
    ugui::StyleC* sc = ui.world().Get<ugui::StyleC>(w);
    if (!sc)
      return;
    ugui::Style s = sc->style;
    mutate(s, arg);
    ugui::SetStyle(ui.world(), w, s);
  }

  void SetVisible(const char* name, bool visible) {
    SetStyleField(
        name,
        [](ugui::Style& s, float v) {
          s.visibility = v > 0.5f ? ugui::Visibility::kVisible : ugui::Visibility::kCollapsed;
        },
        visible ? 1.0f : 0.0f);
  }

  void SetBackground(const char* name, ugui::Color color) {
    ugui::wid w = Need(name);
    if (!w.valid())
      return;
    ugui::StyleC* sc = ui.world().Get<ugui::StyleC>(w);
    if (!sc)
      return;
    ugui::Style style = sc->style;
    style.background = color;
    ugui::SetStyle(ui.world(), w, style);
  }

  void SetTextColor(const char* name, ugui::Color color) {
    ugui::wid w = Need(name);
    if (!w.valid())
      return;
    ugui::StyleC* sc = ui.world().Get<ugui::StyleC>(w);
    if (!sc)
      return;
    ugui::Style style = sc->style;
    style.text_color = color;
    ugui::SetStyle(ui.world(), w, style);
  }

  void SetBorderColor(const char* name, ugui::Color color) {
    ugui::wid w = Need(name);
    if (!w.valid())
      return;
    ugui::StyleC* sc = ui.world().Get<ugui::StyleC>(w);
    if (!sc)
      return;
    ugui::Style style = sc->style;
    style.border_color = color;
    ugui::SetStyle(ui.world(), w, style);
  }

  // Guarded on change: this is a whole-tree FindWidget sweep and the domain
  // changes about once per session.
  void ApplyDomainAccent(u32 rgba) {
    if (rgba == accent_applied)
      return;
    accent_applied = rgba;
    for (const char* n : kAccentFills)
      SetBackground(n, Rgba(rgba));
    for (const char* n : kAccentBorders)
      SetBorderColor(n, Rgba(rgba));
  }
  u32 accent_applied = 0;  // 0 = never applied, so the first call always runs

  // Drives every editor widget from the EditorView each frame. Collapses the
  // whole overlay when the editor is off.
  void ApplyEditorView();

  // Climbs from a clicked widget to the nearest editor-handled name and forwards
  // the matching event to editor_sink. Returns true if it consumed the click.
  bool RouteEditorClick(ugui::wid target);

  // Shows whichever translated screen belongs to the state the UI is in, and
  // hides the rest. Without this every loaded screen draws at once, which is
  // fine for a single screenshot and useless for actually playing.
  void ApplyVanillaVisibility() {
    for (const ui::VanillaScreen& screen : VanillaScreens()) {
      bool show = false;
      switch (VanillaRoleOf(screen.name)) {
        case VanillaRole::kFrontMenu:
          show = main_menu_open;
          break;
        case VanillaRole::kPauseMenu:
          show = menu_open;
          break;
        case VanillaRole::kHud:
          show = !main_menu_open && !menu_open;
          break;
      }
      SetVisible(VanillaRootName(screen.name).c_str(), show);
    }
  }

  // Whether a translated screen has taken over one of the host's own screens,
  // so the host's version stays out of the way instead of drawing underneath.
  bool VanillaCovers(VanillaRole role) const {
    for (const ui::VanillaScreen& screen : VanillaScreens())
      if (VanillaRoleOf(screen.name) == role)
        return true;
    return false;
  }

  void ApplyMenuVisibility() {
    if (VanillaCovers(VanillaRole::kPauseMenu)) {
      SetVisible("menu", false);  // the journal is the pause menu now
      ApplyVanillaVisibility();
      return;
    }
    SetStyleField(
        "menu",
        [](ugui::Style& s, float v) {
          s.visibility = v > 0.5f ? ugui::Visibility::kVisible : ugui::Visibility::kCollapsed;
        },
        menu_open ? 1.0f : 0.0f);
    // Settings and Stats are sub-pages: the entry list stays put and only the
    // pane beside it swaps. The marker rail follows whichever entry owns the
    // visible one.
    const bool root = !settings_open && !stats_open;
    SetVisible("menu_buttons", true);
    SetVisible("menu_detail", root);
    SetVisible("menu_settings", settings_open);
    SetVisible("menu_stats", stats_open);
    SetVisible("btn_resume_mk", root);
    SetVisible("btn_settings_mk", settings_open);
    SetVisible("btn_stats_mk", stats_open);
    ugui::SetText(ui.FindWidget("menu_title"),
                  settings_open ? "Settings" : (stats_open ? "Stats" : "Paused"));
  }

  // Fills the Stats pane's fixed row pool from the current page and hides the
  // leftovers, so a short last page does not leave the previous one's rows up.
  void ApplyStatsPage() {
    const size_t total = stats_view.rows.size();
    const size_t pages = total == 0 ? 1 : (total + kStatsRows - 1) / kStatsRows;
    if (stats_page >= pages)
      stats_page = pages - 1;
    for (size_t i = 0; i < kStatsRows; ++i) {
      const size_t at = stats_page * kStatsRows + i;
      const base::String row = "stat_" + base::ToString(int(i));
      const bool used = at < total;
      SetVisible(row.c_str(), used);
      if (!used)
        continue;
      SetText((row + "_lbl").c_str(), stats_view.rows[at].label.c_str());
      SetText((row + "_val").c_str(), stats_view.rows[at].value.c_str());
    }
    SetVisible("stats_empty", total == 0);
    SetVisible("stats_pager", total > kStatsRows);
    SetText("stats_page",
            (base::ToString(int(stats_page + 1)) + " / " + base::ToString(int(pages))).c_str());
    SetText("stats_count", total == 0 ? "" : base::ToString(int(total)).c_str());
  }

  // --- .ugui hot reload -----------------------------------------------------
  // Snapshot each fragment's last-write time so FragmentsChanged() can detect an
  // edit. A missing file records a default time and simply won't trigger.
  void CaptureFragmentMtimes() {
    fragment_mtimes.clear();
    for (const char* name : kUiFragments) {
      std::error_code ec;
      fragment_mtimes.push_back(fs::last_write_time(UiDir() / name, ec));
    }
  }
  bool FragmentsChanged() const {
    const size_t n = sizeof(kUiFragments) / sizeof(*kUiFragments);
    for (size_t i = 0; i < n && i < fragment_mtimes.size(); ++i) {
      std::error_code ec;
      const auto t = fs::last_write_time(UiDir() / kUiFragments[i], ec);
      if (!ec && t != fragment_mtimes[i])
        return true;
    }
    return false;
  }
  // The message the game sends a menu when it opens it. Most screens fill
  // themselves from their own onLoad; the start menu is the one that waits to
  // be told what the build offers, and the arguments are positional (see
  // StartMenu::setupMainMenu in the decompiled script beside the screen).
  void OpenVanillaScreen(ui::VanillaRuntime& runtime, base::StringRef name) {
    // Every screen is told what it is running on before anything else. A list
    // that has not been told dims and re-lays itself for a controller, which
    // leaves most of its rows at zero alpha on a PC.
    base::Vector<swf::AsValue> platform;
    platform.push_back(swf::AsValue::Number(0));  // PC
    platform.push_back(swf::AsValue::Bool(false));
    runtime.CallRoot(ui, "SetPlatform", platform);

    if (name == "quest_journal") {
      // What the game sends when the journal is opened with Escape rather than
      // the journal key: the System page, with the other tabs locked out. That
      // is the pause menu.
      base::Vector<swf::AsValue> tab;
      tab.push_back(swf::AsValue::Number(2));  // Quest_Journal.PAGE_SYSTEM
      tab.push_back(swf::AsValue::Bool(true));
      runtime.Send(ui, "RestoreSavedSettings", tab);
      return;
    }
    if (name != "startmenu")
      return;
    const bool has_save = false;  // no save browser wired to the vanilla frame yet
    base::Vector<swf::AsValue> args;
    for (int i = 0; i < 15; ++i)
      args.push_back(swf::AsValue::Bool(false));
    args[0] = swf::AsValue::Bool(true);      // quit is offered
    args[1] = swf::AsValue::Bool(has_save);  // continue, and load is enabled
    // The build, which the screen prints as "v <this>" in its corner. Left as
    // the `false` the loop fills with, that is what it prints.
    args[2] = swf::AsValue::Str("recreation");
    args[7] = swf::AsValue::Bool(true);  // mod manager
    args[9] = swf::AsValue::Bool(true);  // no login screen is needed
    runtime.Send(ui, "sendMenuProperties", args);
  }

  // The same conversation for an ActionScript 3 screen. It asks through the
  // code object the game hands it, one call one answer, rather than through
  // GameDelegate's two-way registration.
  static swf::As3Value AnswerVanillaAs3(void* user, swf::Avm2& vm, base::StringRef name,
                                        const base::Vector<swf::As3Value>& args) {
    Impl* self = static_cast<Impl*>(user);
    // What this build offers. Each of these adds a row to the main menu, and
    // answering nothing at all leaves the menu deciding on `undefined`.
    if (name == "GetHasSavedGames")
      return swf::As3Value::Bool(false);
    if (name == "GetHasInstalledContent")
      return swf::As3Value::Bool(false);
    if (name == "GetShowBethesdaNetOption")
      return swf::As3Value::Bool(false);
    if (name == "GetShowCreationClubOption")
      return swf::As3Value::Bool(false);
    if (name == "GetVersionString")
      return swf::As3Value::Str("recreation");
    if (name == "StartNewGame") {
      if (!self->main_menu_open)
        return swf::As3Value::Undefined();
      self->mm_request.kind = MainMenuRequest::Kind::kEnterUniverse;
      self->mm_request.universe = 1;  // Fallout 4
      self->mm_request.multiplayer = false;
      return swf::As3Value::Undefined();
    }
    if (name == "QuitToDesktop" || name == "ExitGame") {
      if (self->menu_open || self->main_menu_open)
        self->quit_requested = true;
      return swf::As3Value::Undefined();
    }
    (void)vm;
    (void)args;
    return swf::As3Value::Undefined();
  }

  // What the game answers when a screen asks it something. A menu leaves the
  // blank on screen until the answer arrives, so an unanswered call is a
  // visible gap: without RequestPlayerInfo the journal's bottom bar keeps
  // reading "Time, Date, Year".
  //
  // This runs from inside the movie's own call, which is the only time an
  // answer can land (see GameBridge::set_answerer). It is also where a menu's
  // decisions arrive: a screen does not tell the host which row was picked, it
  // asks for the thing that row means (StartNewGame, QuitToMainMenu), so acting
  // on those is the whole of "the menu works" without the host knowing what a
  // row is. Fire-and-forget calls (sounds, logging, remembering a tab) are left
  // unhandled on purpose; they collect in GameBridge::pending(), which is the
  // list of what a screen is still waiting on.
  //
  // Each action is guarded on the screen that asks for it being up. A menu says
  // these to its host as part of its own housekeeping while it starts, and
  // acting on one then takes the screen down before the player has seen it.
  static bool AnswerVanillaCall(void* user, swf::GameBridge& bridge,
                                const swf::GameBridge::Call& call) {
    Impl* self = static_cast<Impl*>(user);
    base::Vector<swf::AsValue> answer;
    if (call.name == "RequestPlayerInfo") {
      answer.push_back(swf::AsValue::Str(ui::TamrielDate(self->mm_stats.game_days)));
      answer.push_back(swf::AsValue::Str(base::ToString(self->mm_stats.level)));
      answer.push_back(swf::AsValue::Number(0));
    } else if (call.name == "ShouldShowMod") {
      // No Creation Club and no mod manager here, and answering yes puts a
      // second CREATIONS row in the list beside the one the page authored.
      answer.push_back(swf::AsValue::Bool(false));
    } else if (call.name == "ShouldShowKinectTunerOption") {
      answer.push_back(swf::AsValue::Bool(false));
    } else if (call.name == "SetVersionText") {
      // The odd one out: the movie hands over its own field rather than asking
      // for a string, so the answer is written straight into it.
      if (!call.args.empty() && call.args[0].is_object())
        bridge.vm().SetMember(call.args[0], "text", swf::AsValue::Str("recreation"));
      return true;
    } else if (call.name == "StartNewGame") {
      if (!self->main_menu_open)
        return false;
      self->mm_request.kind = MainMenuRequest::Kind::kEnterUniverse;
      self->mm_request.universe = 0;  // Skyrim
      self->mm_request.multiplayer = false;
      return true;
    } else if (call.name == "QuitToDesktop" || call.name == "ExitGame") {
      if (!self->menu_open && !self->main_menu_open)
        return false;
      self->quit_requested = true;
      return true;
    } else if (call.name == "QuitToMainMenu") {
      if (!self->menu_open)
        return false;
      self->return_to_menu = true;
      self->menu_open = false;
      self->ApplyMenuVisibility();
      return true;
    } else if (call.name == "CloseMenu") {
      // Only when there is a menu to close. A journal says this to its host as
      // part of its own housekeeping while it starts up, and acting on that
      // takes the screen down before the player has seen it.
      if (!self->menu_open)
        return false;
      self->menu_open = false;
      self->ApplyMenuVisibility();
      return true;
    } else {
      return false;
    }
    return bridge.Respond(call.id, answer);
  }

  void StartVanillaVms() {
    vanilla_vms.clear();
    if (!ui::VanillaRuntime::Enabled())
      return;
    const base::String dir = ui::VanillaScreenDir();
    for (const ui::VanillaScreen& screen : VanillaScreens()) {
      ui::VanillaRuntime runtime;
      runtime.SetStrings(&vanilla_strings);
      runtime.SetAnswerer(&Impl::AnswerVanillaCall, this);
      runtime.SetAs3Answerer(&Impl::AnswerVanillaAs3, this);
      if (!runtime.Load(ui, dir, screen.name, VanillaRootName(screen.name)))
        continue;
      OpenVanillaScreen(runtime, screen.name);
      vanilla_vms.push_back(base::move(runtime));
    }
  }

  // Reassemble the tree from the (edited) fragments and reapply the live
  // visibility state the rebuild reset to markup defaults. Per-frame value
  // updates (HUD text, editor view, main-menu data) refresh the rest next frame.
  void ReloadUi() {
    const base::String doc = BuildUi();
    ui.LoadUiString(doc.c_str(), "hud");
    BindVanillaScreens(ui, backend);
    CaptureFragmentMtimes();
    // The tree came back from markup, so the vanilla menus are placeholder
    // frames again and the legal notice is visible again (its markup has no
    // `visibility`, and ugui ignores that property anyway). The screens have to
    // run over the new tree, and the notice has to go back to whatever state it
    // was actually in.
    StartVanillaVms();
    SetVisible("legal", legal_open);
    const bool hud = !editor.active && !UsingVanillaUi();
    SetVisible("topbar", hud);
    SetVisible("crosshair", hud);
    SetVisible("vitals", hud);
    SetVisible("readout", hud);
    SetVisible("editor_root", editor.active);
    editor_prev_active = editor.active;
    ApplyMenuVisibility();
    ApplyStatsPage();
    ApplyMainMenu();
    ApplyFirstRun();
    RX_INFO("ui: hot-reloaded {} .ugui fragment(s)", sizeof(kUiFragments) / sizeof(*kUiFragments));
  }
};


}  // namespace rx

#endif  // RECREATION_HAS_UGUI
#endif  // RUNTIME_UI_GAME_UI_INTERNAL_H
