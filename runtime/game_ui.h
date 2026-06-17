#ifndef RECREATION_RUNTIME_GAME_UI_H_
#define RECREATION_RUNTIME_GAME_UI_H_

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
  // The dialogue panel (speaker line + NPC reply + numbered player topics).
  void SetDialogue(const DialogueView& dialogue);

  void ToggleMenu();
  bool menu_open() const;
  bool quit_requested() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rec

#endif  // RECREATION_RUNTIME_GAME_UI_H_
