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
constexpr float kEditorBrowserWidth = 360.0f;    // left asset-browser dock
constexpr float kEditorToolbarHeight = 46.0f;    // top toolbar
constexpr float kEditorInspectorWidth = 300.0f;  // right inspector dock
constexpr float kEditorStatusHeight = 34.0f;     // bottom status bar
constexpr int kEditorBrowserRows = 14;           // visible asset rows per page
constexpr int kEditorCategoryTabs = 12;          // pooled category-tab widgets
constexpr int kEditorToolButtons = 8;            // toolbar action buttons

// The map editor's whole screen state, rebuilt each frame by MapEditor and
// mirrored into the editor overlay. Empty/!active hides every editor panel.
struct EditorView {
  bool active = false;
  bool browser_open = true;
  int tool = 0;  // 0 select, 1 move, 2 rotate, 3 scale (matches MapEditor::Tool)

  // Asset browser (left dock).
  std::vector<std::string> categories;  // tab labels ("All", "Statics", ...)
  int category = 0;                     // selected tab
  std::string search;                   // text in the search box ("" shows hint)
  bool search_focused = false;          // search box has keyboard focus
  struct AssetRow {
    std::string name;      // FULL name or editor id
    std::string subtitle;  // "TYPE  editor_id"
    bool armed = false;    // chosen as the place brush
  };
  std::vector<AssetRow> rows;  // the visible page of filtered results
  int result_count = 0;        // total matches in the filter
  int page_first = 0;          // index of rows[0] within the filtered set

  // Selection inspector (right dock).
  bool has_selection = false;
  std::string sel_title;
  std::string sel_subtitle;
  float sel_pos[3] = {0, 0, 0};
  float sel_yaw_deg = 0;
  float sel_scale = 1;
  // Screen-space selection bracket: where the selected object projects to (in
  // window pixels) and its projected half-size, so the overlay can frame it.
  bool sel_on_screen = false;
  float sel_screen[2] = {0, 0};
  float sel_screen_half = 40;

  // Status bar (bottom).
  std::string brush;     // armed asset name ("" = none, in select mode)
  std::string status;    // transient hint / confirmation message
  int object_count = 0;  // editor-placed objects so far
};

// A click (or scroll) inside the editor overlay, forwarded from the UI to
// MapEditor which owns all the logic. `index` meaning depends on the kind.
struct EditorUiEvent {
  enum class Kind {
    kPickRow,      // index = visible row clicked (0..rows.size())
    kCategory,     // index = category tab clicked
    kTool,         // index = toolbar action (see MapEditor)
    kScroll,       // index = +1 page down / -1 page up
    kCloseBrowser  // index unused
  };
  Kind kind = Kind::kPickRow;
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

  void ToggleMenu();
  bool menu_open() const;
  bool quit_requested() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rec

#endif  // RECREATION_RUNTIME_GAME_UI_H_
