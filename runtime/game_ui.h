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

  // Hide or show the gameplay HUD (compass, crosshair, vitals, readout) without
  // touching the pause menu. The cinematic showcase hides it for clean frames.
  void SetHudVisible(bool visible);

  // Map editor overlay: the asset browser, toolbar, inspector and status bar.
  // SetEditorView replaces the whole editor screen state each frame (inactive
  // hides every editor panel and restores the gameplay HUD). SetEditorEventSink
  // registers the callback that receives clicks/scrolls on the editor widgets.
  void SetEditorView(const EditorView& view);
  void SetEditorEventSink(std::function<void(const EditorUiEvent&)> sink);

  // Registers an RGBA8 image (width*height*4 bytes) with the UI's texture backend
  // and returns its ugui TextureId, for editor asset-card thumbnails. Returns 0
  // when the UI is unavailable. The id stays valid until shutdown.
  u64 CreateUiTexture(int width, int height, const u8* rgba);

  void ToggleMenu();
  bool menu_open() const;
  bool quit_requested() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rec

#endif  // RECREATION_RUNTIME_GAME_UI_H_
