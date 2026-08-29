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

  // What the build offers. `has_creations` is the game's ShouldShowMod answer,
  // which splices $CREATIONS in at SystemPage::MOD_MANAGER_BUTTON_INDEX.
  struct Availability {
    bool has_creations = false;
  };

  // Builds the category list against the screen's widgets and puts the System
  // page in front. `strings` resolves the "$KEY" the movie stores. Returns
  // false when the screen is not the journal.
  bool Build(ugui::UIContext& ui,
             const Availability& availability,
             const base::UnorderedMap<base::String, base::String>* strings);

  // Writes the entries into the movie's own rows. Call after the tree is built
  // and whenever the selection moves.
  void Apply(ugui::UIContext& ui);

  void MoveSelection(int delta);
  // Pointer landed on `target`: selects that row when it is one. Returns true
  // when the click belonged to the menu.
  bool HandleClick(ugui::UIContext& ui, u32 target_id);

  Action Selected() const;
  bool empty() const { return list_.empty(); }

  // The bottom bar's date and level readout, which the game fills from
  // RequestPlayerInfo. `game_days` is the WorldClock's fractional day count,
  // rendered onto the Tamrielic calendar the way the journal shows it;
  // `progress` is the fraction to the next level.
  void SetPlayerInfo(ugui::UIContext& ui, f64 game_days, int level, f32 progress);

 private:
  VanillaList list_;
  base::Vector<Action> actions_;  // parallel to the list's entries
  f32 meter_width_ = -1.0f;       // the level meter's authored width
};

}  // namespace rx::ui

#endif  // RECREATION_RUNTIME_UI_VANILLA_PAUSE_MENU_H_
