#ifndef RECREATION_RUNTIME_UI_VANILLA_PAUSE_MENU_H_
#define RECREATION_RUNTIME_UI_VANILLA_PAUSE_MENU_H_

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include "core/types.h"
#include "runtime/ui/vanilla_list.h"

namespace ugui {
class UIContext;
}  // namespace ugui

namespace rx::ui {

// Drives the translated Skyrim pause menu the way the game drives the original.
//
// Skyrim's pause menu is the journal's System page: ESC opens quest_journal.swf
// with the System tab in front (Quest_Journal::RestoreSavedSettings picks the
// tab, SystemPage::startPage puts it in MAIN_STATE). The movie ships all three
// tab pages and all ten of the System page's sub-panels stacked in one frame,
// with "BUTTON TEXT" in the tabs and blank rows in the category list, so a
// static translation renders as an empty frame. This fills the category list
// the way SystemPage::onLoad does, labels the tabs, and puts every panel that
// belongs to another state out of the way.
//
// SystemPage is a state machine over those panels; the states on the path out
// of the game are driven here. Picking QUIT opens the movie's own two-entry
// list (main menu / desktop) rather than quitting outright, which is what the
// original does.
class VanillaPauseMenu {
 public:
  enum class Action : u8 {
    kNone,
    kQuickSave,
    kSave,
    kLoad,
    kInstalledContent,
    kCreations,
    kSettings,
    kControls,
    kHelp,
    kQuit,
  };

  // What the host should do about the last activation.
  enum class Result : u8 {
    kNone,        // stayed put, or moved to a sub-panel
    kCloseMenu,   // back out of the menu entirely
    kMainMenu,    // leave the world, return to the front menu
    kDesktop,     // leave the game
  };

  // What the build offers. `has_creations` is the game's ShouldShowMod answer,
  // which splices $CREATIONS in at SystemPage::MOD_MANAGER_BUTTON_INDEX.
  struct Availability {
    bool has_creations = false;
  };

  // Builds the category list and the sub-panels against the screen's widgets,
  // and puts the System page in front. `strings` resolves the "$KEY" the movie
  // stores. Returns false when the screen is not the journal.
  bool Build(ugui::UIContext& ui,
             const Availability& availability,
             const base::UnorderedMap<base::String, base::String>* strings);

  // Writes the entries into the movie's own rows and shows the panel the
  // current state owns. Call after the tree is built and on every change.
  void Apply(ugui::UIContext& ui);

  void MoveSelection(int delta);
  // Pointer landed on `target`: selects that row when it is one. Returns true
  // when the click belonged to the menu.
  bool HandleClick(ugui::UIContext& ui, u32 target_id);
  // Enter / A on whatever list is up.
  Result Activate(ugui::UIContext& ui);
  // Esc / B. Returns true when it closed a sub-panel, false when the menu
  // itself should close (the state was already the top one).
  bool Back(ugui::UIContext& ui);
  // Puts the menu back on its category list, for the next time it opens.
  void Reset(ugui::UIContext& ui);

  Action Selected() const;
  bool empty() const { return list_.empty(); }

  // The bottom bar's date and level readout, which the game fills from
  // RequestPlayerInfo. `game_days` is the WorldClock's fractional day count,
  // rendered onto the Tamrielic calendar the way the journal shows it;
  // `progress` is the fraction to the next level.
  void SetPlayerInfo(ugui::UIContext& ui, f64 game_days, int level, f32 progress);

 private:
  // The SystemPage states this drives. The rest (save/load, input mapping,
  // help, creations) are panels the movie carries but nothing stands behind.
  enum class State : u8 { kMain, kQuit, kSettings };

  VanillaList& ActiveList();
  const VanillaList& ActiveList() const;

  VanillaList list_;           // the category list
  VanillaList quit_list_;      // main menu / desktop
  VanillaList settings_list_;  // gameplay / display / audio
  base::Vector<Action> actions_;  // parallel to the category list's entries
  State state_ = State::kMain;
  f32 meter_width_ = -1.0f;    // the level meter's authored width
};

}  // namespace rx::ui

#endif  // RECREATION_RUNTIME_UI_VANILLA_PAUSE_MENU_H_
