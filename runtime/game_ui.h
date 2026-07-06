#ifndef RECREATION_RUNTIME_GAME_UI_H_
#define RECREATION_RUNTIME_GAME_UI_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/types.h"

namespace rec {

class Window;
class FlyCamera;
namespace render {
class Renderer;
struct FrameView;
}  // namespace render

// The quest the HUD tracker shows: a title and its displayed objectives. The
// engine fills this from the quest system's running snapshot; an empty title
// hides the tracker.
struct HudQuest {
  struct Objective {
    std::string text;
    bool completed = false;
  };
  std::string title;
  std::vector<Objective> objectives;
};

// A persistent labeled HUD gauge pushed from managed gameplay (oxygen, radiation,
// adrenaline, ...) through the Hud.Gauge native. The engine snapshots the live
// set each frame; an empty list hides the gauge stack.
struct HudGauge {
  std::string id;
  std::string label;
  float fraction = 0;  // 0..1
  u32 color = 0;       // packed rgba8; 0 = HUD default
};

// The open conversation the dialogue panel shows: the speaker, their last line,
// and the numbered player topics to choose from. open == false hides the panel.
struct DialogueView {
  bool open = false;
  std::string speaker;
  std::string npc_line;
  std::vector<std::string> options;
};

// The open container the loot panel shows: its name and the items inside (name
// + count). open == false hides the panel.
struct ContainerView {
  struct Item {
    std::string name;
    int count = 0;
  };
  bool open = false;
  std::string name;
  std::vector<Item> items;
};

// Editor overlay geometry, shared by the layout (game_ui.cc) and the pointer
// hit-test (editor.cc) so a click on a panel never also acts on the world.
// A Creation-Kit-style dock layout: top toolbar, left scene/assets dock, right
// inspector, a bottom asset browser between the side docks, and a status bar.
constexpr float kEdToolbarH = 56.0f;     // top toolbar
constexpr float kEdSceneW = 270.0f;      // left scene/assets dock
constexpr float kEdInspectorW = 314.0f;  // right inspector dock
constexpr float kEdStatusH = 30.0f;      // bottom status bar
constexpr float kEdBrowserH = 214.0f;    // bottom asset-browser dock (above status)
constexpr int kEdTreeRows = 18;          // visible scene-tree rows
constexpr int kEdToolBtns = 9;           // Select Move Rotate Terrain Paint Play Save Undo Redo
constexpr int kEdTabs = 9;               // asset-browser category tabs (== kEditorCategoryCount)
constexpr int kEdCatRows = 9;            // left category list rows in the browser
constexpr int kEdCards = 12;             // visible asset cards per page
constexpr int kEdTags = 5;               // inspector tag chips

// The map editor's whole screen state, rebuilt each frame by MapEditor and
// mirrored into the editor overlay. Empty/!active hides every editor panel.
struct EditorView {
  bool active = false;
  int tool = 0;  // active toolbar tool (0..8), drives the toolbar highlight

  // --- left dock: scene tree / assets tabs ---
  int left_tab = 0;  // 0 = Scene, 1 = Assets
  std::string scene_search;
  bool scene_search_focused = false;
  struct TreeRow {
    int depth = 0;          // indent level (0 root, 1 group, 2 leaf)
    int icon = 0;           // 0 root, 1 group, 2 light, 3 mesh
    std::string name;
    bool selected = false;  // part of the editor selection
    bool hidden = false;    // eye toggled off (world::Hidden)
    int expand = 0;         // 0 leaf, 1 collapsed, 2 expanded
  };
  std::vector<TreeRow> tree;  // the visible window of the flattened tree
  int tree_total = 0;         // total flattened rows (for the pager)

  // --- viewport overlays ---
  int gizmo = 1;  // highlighted gizmo button: 0 hand, 1 move, 2 rotate, 3 scale

  // --- right dock: inspector ---
  bool has_selection = false;
  std::string sel_name;   // object display name
  std::string sel_type;   // record fourcc, e.g. "STAT"
  std::string model_name;
  std::string material_name;
  u64 model_thumb = 0;  // rendered preview of the selected model (0 = none)
  float pos[3] = {0, 0, 0};
  float rot[3] = {0, 0, 0};    // euler degrees (pitch, yaw, roll)
  float scale[3] = {1, 1, 1};
  bool sel_static = false;
  bool cast_shadow = true;
  bool receive_shadow = true;
  bool lightmap_static = false;
  std::vector<std::string> tags;
  int sel_count = 0;  // number of objects selected
  // Screen-space selection bracket (window pixels).
  bool sel_on_screen = false;
  float sel_screen[2] = {0, 0};

  // Marquee box-select rectangle (window pixels). Inactive hides it.
  bool marquee_active = false;
  float marquee[4] = {0, 0, 0, 0};  // x0, y0, x1, y1

  // --- bottom dock: asset browser ---
  std::vector<std::string> tabs;  // category tab labels
  int tab = 0;                    // active tab (== category index)
  std::string asset_search;
  bool asset_search_focused = false;
  struct CatRow {
    std::string name;
    int count = 0;
    bool active = false;
  };
  std::vector<CatRow> cats;
  struct Card {
    std::string name;
    u64 thumb = 0;   // ugui TextureId for the rendered preview (0 = none yet)
    u32 color = 0;   // placeholder swatch colour (rgba8) until the thumb is ready
    bool armed = false;
  };
  std::vector<Card> cards;
  int card_total = 0;  // total filtered entries (for paging)
  int card_first = 0;  // index of cards[0] within the filtered set

  // --- status bar ---
  std::string status;          // "Ready" / transient confirmation
  std::string grid_label = "1 m";
  bool snapping = false;
  int object_count = 0;        // editor-placed objects so far
};

// Character-creation overlay geometry, shared by the layout (game_ui.cc) and the
// C++ hit-testing (chargen.cc) so a click / drag lands on the widget under it.
// Both panels are absolute-positioned at fixed pixel offsets (not flex), so the
// two files agree on every control's rect. Left panel = race / sex / preset /
// page tabs / actions; right panel = the scrollable slider list for the page.
constexpr float kCgTop = 10.0f;         // both panels' top (px)
constexpr float kCgLeftW = 322.0f;      // left panel width
constexpr float kCgRightW = 396.0f;     // right panel width
constexpr float kCgPad = 16.0f;         // panel inner padding
// Left panel: interactive elements at fixed offsets from the panel top.
constexpr float kCgRaceY0 = 86.0f;      // first race row (rel to panel top)
constexpr float kCgRaceRowH = 27.0f;    // race row height
constexpr int kCgRaceRows = 10;         // pooled race rows
constexpr float kCgSexY = 392.0f;       // sex Male/Female buttons (rel)
constexpr float kCgPresetY = 458.0f;    // preset prev/label/next bar (rel)
constexpr float kCgPageY = 524.0f;      // page tabs (rel)
constexpr float kCgActY = 578.0f;       // first action button (rel)
constexpr float kCgActH = 34.0f;        // action button height + gap
constexpr float kCgBtnH = 30.0f;        // small control height
// Right panel: the pooled slider/cycler rows.
constexpr float kCgRowsY0 = 66.0f;      // first row (rel to panel top)
constexpr float kCgRowH = 40.0f;        // one control row height
constexpr int kCgSliderRows = 11;       // pooled slider/cycler rows
constexpr float kCgTrackX = 158.0f;     // track left (rel to right panel left)
constexpr float kCgTrackW = 164.0f;     // track width

// The character-creation screen state, rebuilt each frame by CharGen and mirrored
// into the overlay (mirrors EditorView). Empty / !active hides every panel.
struct CharGenView {
  bool active = false;
  // --- left dock ---
  std::vector<std::string> races;  // playable race display names
  int race = 0;                    // selected race index
  int sex = 0;                     // 0 male, 1 female
  std::string preset_label;        // e.g. "Preset 2 / 6"
  int page = 0;                    // 0 Face, 1 Advanced, 2 Appearance
  std::string status;              // transient confirmation ("Saved ...")
  // --- right dock: the active page's controls ---
  std::string page_title;
  struct Row {
    std::string label;
    std::string value;    // formatted value / index text
    float fill = 0;       // 0..1 track fill (sliders); cyclers centre at 0.5
    int kind = 0;         // 0 slider, 1 cycler
    u32 swatch = 0;       // small colour chip (rgba8); 0 = none
  };
  std::vector<Row> rows;  // the visible window of the page's controls
  int row_total = 0;      // total controls on the page (for the pager)
  int row_first = 0;      // index of rows[0] within the page's control list
};

// Live data the engine feeds the NEXUS main menu each frame: the player banner,
// the network status line, and the per-column "available" flags. Read by the
// menu's profile/multiplayer/status widgets; all optional, sensible defaults.
struct MainMenuStats {
  std::string player_name;     // the local profile handle (account or --name)
  int level = 0;
  bool in_game = false;        // a universe is loaded and being played
  std::string universe;        // loaded game's display name (empty = none yet)
  std::string location;        // current cell/region, when in game
  float health = 1, magicka = 1, stamina = 1;  // 0..1 vitals, when in game
  int gold = 0;
  int active_quests = 0;
  int players_online = 0;      // connected peers (host or client), 0 = offline
  std::string net_status;      // "Offline" / "Hosting :29700" / "Connected ..."

  // Real local-profile / system identity, shown on the front screen before any
  // universe is loaded (the profile is the machine account, not an RPG hero).
  std::string account;         // OS login name
  std::string machine;         // hostname
  std::string build;           // engine version string
  int universes_available = 0; // detected, playable universes
};

// One entry on the menu's NEWS rail, parsed from CHANGELOG.md: a short headline
// and a detail line (version + date).
struct MenuNewsItem {
  std::string title;
  std::string detail;
};

// A request the main menu raises for the engine to act on. The engine polls it
// once per frame (PollMainMenuRequest) and clears it by consuming. Loading a
// universe makes that game the primary content domain, which boots its C#
// gameplay module (SkyrimMod / Fallout / StarfieldMod gate on the primary).
struct MainMenuRequest {
  enum class Kind { kNone, kEnterUniverse, kHostServer, kJoinServer, kQuit, kOpenUrl };
  Kind kind = Kind::kNone;
  int universe = 0;         // 0 Skyrim, 1 Fallout 4, 2 Starfield
  std::string address;      // join target ("ip[:port]"), for kJoinServer
  std::string url;          // external link to open, for kOpenUrl
  bool multiplayer = false; // kEnterUniverse also opened a session
};

// Live data the engine feeds the first-run setup wizard each frame: the three
// games' display names, whether each has been located (and where), the chosen
// mods directory, and the recommended free-space label. The wizard owns its own
// step and selection state. This only mirrors what the engine resolved or browsed.
struct FirstRunView {
  struct Game {
    std::string name;     // display name (e.g. "Skyrim Special Edition")
    std::string path;     // located data dir, empty if not found
    bool located = false;
  };
  std::vector<Game> games;        // up to three, column order
  std::string mods_dir;           // current mods directory
  std::string space_label = "50 GB";
};

// A request the first-run wizard raises for the engine to act on, mirroring
// MainMenuRequest: open a native folder picker for a game or the mods dir, or
// finish (kLaunch, persisting the choices snapshot below) / cancel the setup.
struct FirstRunRequest {
  enum class Kind { kNone, kBrowseGame, kBrowseMods, kLaunch, kCancel };
  Kind kind = Kind::kNone;
  int index = 0;                  // game column, for kBrowseGame
  // Snapshot of the wizard's selections, filled on kLaunch so the engine can
  // persist them (these have no effect on the other request kinds).
  int mode = 0;                   // 0 Exploration, 1 Story, 2 Survival, 3 Sandbox
  int difficulty = 1;             // 0 Novice, 1 Normal, 2 Hard, 3 Legendary
  bool enable_mods = true;
  bool share_diagnostics = true;
  bool check_updates = true;
};

// The rebindable controls shown in the pause menu's Settings sub-view. The
// engine rebuilds this from its InputMap each frame; the rows are a fixed,
// curated set of gameplay actions. A row marked `capturing` is awaiting an input
// to bind ("Press any key / button...").
struct ControlsRow {
  std::string label;    // action display name, e.g. "Jump"
  std::string binding;  // current binding label, or the capture prompt
  bool capturing = false;
};
struct ControlsView {
  std::vector<ControlsRow> rows;
  std::string sens_kbm;     // formatted mouse look sensitivity
  std::string sens_pad;     // formatted gamepad look sensitivity
  bool invert_y = false;
  bool gamepad = false;     // a pad is connected (drives glyph hints)
};

// A request the Settings sub-view raises for the engine (which owns the
// InputMap) to act on. Polled once per frame and cleared on consume, mirroring
// MainMenuRequest.
struct SettingsRequest {
  enum class Kind { kNone, kRebind, kSensKbm, kSensPad, kInvertToggle, kReset, kTestRumble };
  Kind kind = Kind::kNone;
  int row = 0;    // kRebind: index into ControlsView.rows
  int delta = 0;  // kSensKbm / kSensPad: -1 or +1
};

// A click (or scroll) inside the editor overlay, forwarded from the UI to
// MapEditor which owns all the logic. `index` meaning depends on the kind.
struct EditorUiEvent {
  enum class Kind {
    kTool,        // index = toolbar tool (0..8)
    kGizmo,       // index = viewport gizmo mode (0..3)
    kLeftTab,     // index = 0 scene / 1 assets
    kTreeSelect,  // index = visible tree row
    kTreeEye,     // index = visible tree row (toggle visibility)
    kTreeExpand,  // index = visible tree row (toggle expand)
    kTreeScroll,  // index = +1 page down / -1 page up
    kCategory,    // index = category (tab or left list)
    kPickCard,    // index = visible card
    kCardScroll,  // index = +1 page / -1 page
    kFocusScene,
    kClearScene,
    kFocusAsset,
    kClearAsset,
    kSnapToggle,
    kGridCycle,
  };
  Kind kind = Kind::kTool;
  int index = 0;
};

// libultragui-driven HUD and pause menu. Runs ultragui in draw-data mode and
// records its draw list into the renderer's ui pass, alongside the debug ImGui
// overlay. Compiles to a stub when ultragui is unavailable (RECREATION_HAS_UGUI
// off, e.g. the dedicated server build).
class GameUi {
 public:
  GameUi();
  ~GameUi();

  GameUi(const GameUi&) = delete;
  GameUi& operator=(const GameUi&) = delete;

  bool Initialize(Window& window, render::Renderer& renderer);
  // Call between renderer WaitIdle and renderer Shutdown.
  void Shutdown();

  // Feed input, drive HUD values from engine state, produce the draw list and
  // fill view->hud_draw. Call once per frame after PumpEvents.
  void Build(Window& window, render::Renderer& renderer, FlyCamera& camera, f32 frame_delta,
             render::FrameView* view);

  // Quest HUD, fed by the engine each frame. SetQuest replaces the tracked
  // quest (empty title hides the tracker). FlashQuestUpdate shows a brief
  // "quest updated" banner. SetActivatePrompt shows a centered prompt such as
  // "Talk to Ralof" (empty hides it).
  void SetQuest(const HudQuest& quest);
  // Persistent managed gameplay gauges (oxygen, radiation, ...), shown as a
  // labeled bar stack above the vitals. Replaces the whole set each frame.
  void SetHudGauges(const std::vector<HudGauge>& gauges);
  // The multiplayer chat box lines (newest last); the box shows the last several
  // and collapses when empty. Fed from the platform chat channel each frame.
  void SetChatLines(const std::vector<std::string>& lines);
  // The multiplayer scoreboard (hold-Tab player list). `open` shows the centered
  // panel; `header` is the column header line and `rows` the pre-formatted player
  // rows. Fed from the platform scoreboard channel each frame.
  void SetScoreboard(bool open, const std::string& title, const std::string& header,
                     const std::vector<std::string>& rows);
  // Multiplayer interaction prompts (already formatted, e.g. "[E]  Open"), shown
  // as a bottom-centre stack. Fed from the platform prompt channel each frame.
  void SetPrompts(const std::vector<std::string>& prompts);
  // Map blips placed on the compass: each is a bearing (0 ahead, + to the right of
  // where the player looks) and a packed rgba8 colour. Off-screen ones are dropped
  // by the caller. Fed from the platform map channel each frame.
  struct CompassBlip {
    float bearing_deg = 0;
    u32 color = 0xffffffffu;
  };
  void SetCompassBlips(const std::vector<CompassBlip>& blips);
  // Floating world-space labels (player nametags) already projected to screen
  // pixels by the engine. Fed from the platform nametag channel each frame.
  struct Nametag {
    std::string label;
    float sx = 0, sy = 0;  // screen position (pixels)
    u32 color = 0xffffffffu;
  };
  void SetNametags(const std::vector<Nametag>& nametags);
  void FlashQuestUpdate(const std::string& message);
  void SetActivatePrompt(const std::string& prompt);
  // Objective compass waypoint. active shows a pip on the compass at
  // bearing_deg (0 = dead ahead, positive = to the right of where the player
  // looks) and a distance readout; inactive hides both.
  void SetObjectiveMarker(bool active, float bearing_deg, float distance_m);
  // The dialogue panel (speaker line + NPC reply + numbered player topics).
  void SetDialogue(const DialogueView& dialogue);
  // The container loot panel (a chest/barrel's name and the items inside).
  void SetContainer(const ContainerView& container);
  // Quest journal overlay: `open` shows a numbered list of the player's active
  // quests; `selected` (a 0-based index into `quests`, or < 0) highlights the
  // tracked one and lists its objectives. Pressing its number pins that quest.
  void SetJournal(bool open, const std::vector<HudQuest>& quests, int selected);

  // War-map overlay: the Civil War campaign board. `open` shows the panel; each
  // hold carries its name and owner (0 neutral, 1 Imperial, 2 Stormcloak), and
  // `imperial_fraction` (0..1) drives the war-progress bar. Pushed by the managed
  // Civil War campaign and snapshotted onto the panel.
  struct WarHoldEntry {
    std::string name;
    int owner = 0;
  };
  void SetWarMap(bool open, const std::vector<WarHoldEntry>& holds, float imperial_fraction);

  // Hide or show the gameplay HUD (compass, crosshair, vitals, readout) without
  // touching the pause menu. The cinematic showcase hides it for clean frames.
  void SetHudVisible(bool visible);

  // Map editor overlay: the asset browser, toolbar, inspector and status bar.
  // SetEditorView replaces the whole editor screen state each frame (inactive
  // hides every editor panel and restores the gameplay HUD). SetEditorEventSink
  // registers the callback that receives clicks/scrolls on the editor widgets.
  void SetEditorView(const EditorView& view);
  void SetEditorEventSink(std::function<void(const EditorUiEvent&)> sink);

  // Character-creation overlay: the race / sex / preset / page controls and the
  // scrollable slider list. CharGen owns all the interaction (it hit-tests its own
  // fixed-geometry panels against the raw cursor), so this is a pure view push;
  // an inactive view hides every chargen panel and restores the gameplay HUD.
  void SetCharGenView(const CharGenView& view);

  // Registers an RGBA8 image (width*height*4 bytes) with the UI's texture backend
  // and returns its ugui TextureId, for editor asset-card thumbnails. Returns 0
  // when the UI is unavailable. The id stays valid until shutdown.
  u64 CreateUiTexture(int width, int height, const u8* rgba);

  void ToggleMenu();
  bool menu_open() const;
  bool settings_open() const;  // pause menu's Settings sub-view is showing
  bool quit_requested() const;

  // Pause-menu Settings sub-view: the engine pushes the current controls each
  // frame and polls the request the panel raises (rebind a row, nudge a
  // sensitivity, toggle invert, reset, test rumble).
  void SetControlsView(const ControlsView& view);
  SettingsRequest PollSettingsRequest();

  // NEXUS main menu (the startup "choose your universe" screen). Distinct from
  // the in-game pause menu above. The engine opens it at boot, drives it with
  // mouse + the keyboard helpers below, feeds it live data, and polls the
  // request it raises (enter a universe, host/join, quit).
  void OpenMainMenu();
  void CloseMainMenu();
  bool main_menu_open() const;
  // Keyboard navigation: dy moves the left-nav selection (-1 up / +1 down), dx
  // moves the universe selection (-1 left / +1 right). Activate triggers the
  // current nav item (PLAY enters the selected universe); Back closes a
  // sub-screen (returns true if it consumed the press, false at the root).
  void MainMenuMove(int dx, int dy);
  void MainMenuActivate();
  bool MainMenuBack();
  bool MainMenuAtRoot() const;
  // The names/availability of the three universes (greyed out if data is
  // missing), the per-column live backdrop texture, the player/network banner,
  // and the loaded C# mod list shown on the Mods screen. All optional.
  void SetMainMenuUniverses(const std::vector<std::string>& names,
                            const std::vector<bool>& available);
  void SetMainMenuBackdrop(int universe, u64 texture);
  // Bind a procedurally-painted emblem texture to a named image widget in the
  // menu (e.g. "gl_nexus", "gl_skyrim"). Rebound each frame so it survives a
  // hot-reload rebuild of the widget tree.
  void SetMainMenuGlyph(const std::string& widget, u64 texture);
  void SetMainMenuStats(const MainMenuStats& stats);
  void SetMainMenuMods(const std::vector<std::string>& mods);
  // The NEWS rail entries (most-recent first), parsed from CHANGELOG.md.
  void SetMainMenuNews(const std::vector<MenuNewsItem>& news);
  // The universe column currently selected (0 Skyrim, 1 Fallout 4, 2 Starfield).
  int selected_universe() const;
  // Consume the pending request (kNone if none). Called by the engine each frame.
  MainMenuRequest PollMainMenuRequest();

  // First-run setup wizard (the out-of-box experience shown once on a fresh
  // install, before the main menu). The engine opens it at boot when setup has
  // not completed, drives it with mouse + the Next/Back helpers below, feeds it
  // the located games / mods dir each frame, and polls the request it raises
  // (browse for a folder, launch, cancel).
  void OpenFirstRun();
  void CloseFirstRun();
  bool first_run_open() const;
  // Keyboard helpers: Next advances the current page (the primary button, which
  // is gated on locating a game on page 2); Back returns to the previous page,
  // or raises kCancel at the first page.
  void FirstRunNext();
  void FirstRunBack();
  void SetFirstRunView(const FirstRunView& view);
  FirstRunRequest PollFirstRunRequest();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rec

#endif  // RECREATION_RUNTIME_GAME_UI_H_
