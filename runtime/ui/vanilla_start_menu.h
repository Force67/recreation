#ifndef RECREATION_RUNTIME_UI_VANILLA_START_MENU_H_
#define RECREATION_RUNTIME_UI_VANILLA_START_MENU_H_

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include "core/types.h"

namespace ugui {
class UIContext;
struct WidgetId;
}  // namespace ugui

namespace rx::ui {

// Drives the translated Skyrim start menu the way the game drives the original.
//
// The movie ships the menu as an empty frame: its option rows are authored
// transparent with the placeholder "OPTION" in them, and the game fills them on
// open. StartMenu::setupMainMenu (see the decompiled script beside the screen)
// pushes $CONTINUE, $NEW, $LOAD, then whatever the build has, then $CREDITS and
// $QUIT. This builds the same list, writes it into the same widgets, and moves
// the same selection arrow.
class VanillaStartMenu {
 public:
  enum class Action : u8 {
    kNone,
    kContinue,
    kNew,
    kLoad,
    kCreations,
    kMods,
    kCredits,
    kQuit,
  };

  // What the build offers, matching the arguments the game passes to
  // setupMainMenu. `has_save` is what makes CONTINUE appear and LOAD usable.
  struct Availability {
    bool has_save = false;
    bool can_quit = true;
    bool has_mods = true;
    bool has_creations = false;
  };

  // Rebuilds the option list. `strings` resolves the "$KEY" the movie stores.
  void Build(const Availability& availability,
             const base::UnorderedMap<base::String, base::String>* strings);

  // Writes the options into the movie's own row widgets, spaces them the way the
  // list component does, and parks the selection arrow. Call after the tree is
  // built and whenever the selection moves.
  void Apply(ugui::UIContext& ui);

  // Keyboard/pad movement over the enabled rows.
  void MoveSelection(int delta);
  // Pointer landed on `target`: selects that row when it is one. Returns true
  // when the click belonged to the menu.
  bool HandleClick(ugui::UIContext& ui, u32 target_id);

  Action Selected() const;
  bool empty() const { return options_.empty(); }

 private:
  struct Option {
    Action action = Action::kNone;
    base::String text;
    bool disabled = false;
    u32 row_id = 0;  // the widget this option was written into
  };

  base::Vector<Option> options_;
  mem_size selected_ = 0;
};

}  // namespace rx::ui

#endif  // RECREATION_RUNTIME_UI_VANILLA_START_MENU_H_
