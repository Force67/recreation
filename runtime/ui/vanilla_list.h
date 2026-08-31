#ifndef RECREATION_RUNTIME_UI_VANILLA_LIST_H_
#define RECREATION_RUNTIME_UI_VANILLA_LIST_H_

#include <base/containers/vector.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include "core/types.h"

namespace ugui {
class UIContext;
}  // namespace ugui

namespace rx::ui {

// One of the games' own scrolling lists (Shared.BSScrollingList and friends),
// driven from the host the way the movie's component drives it.
//
// Every list in the shipped movies is authored empty: the entry clips hold a
// placeholder label, and on open the game pushes the real entries in and calls
// UpdateList, which stacks the clips from the first one's position, writes each
// entry's text and puts the selected one in its highlight state. A translated
// screen that skips this renders as a blank frame, so this replays it against
// the widgets the translation produced.
class VanillaList {
 public:
  struct Entry {
    base::String text;
    bool disabled = false;
  };

  // Finds `holder` by the name the movie gave it and takes the list inside it
  // (the components all call it "List_mc"), then collects the entry rows: the
  // children carrying a label, which excludes the list's `border` spacer.
  // Returns false when the screen has no such list.
  bool Bind(ugui::UIContext& ui, base::StringRef holder, base::StringRef list = "List_mc");

  void SetEntries(base::Vector<Entry> entries);

  // Writes the entries into the rows and stacks them. Rows past the last entry
  // go transparent, which is what the component does with its spare clips.
  void Apply(ugui::UIContext& ui);

  void MoveSelection(int delta);
  // Selects the row `target_id` belongs to. Returns true when it is one, so a
  // caller can tell a click on the list from a click past it.
  bool HandleClick(ugui::UIContext& ui, u32 target_id);

  mem_size selected() const { return selected_; }
  bool empty() const { return entries_.empty(); }
  bool bound() const { return !holder_name_.empty(); }

  // The selected row's box within the list, for a caller parking a selection
  // arrow beside it.
  f32 selected_top() const { return selected_top_; }
  f32 selected_height() const { return selected_height_; }
  f32 list_top() const { return list_top_; }

  // Dress the rows in the component's own state frames rather than in the
  // geometry each happened to be captured with. BSScrollingList::SetEntry sends
  // the highlighted clip to a taller "Selected" frame and the rest to "Normal",
  // so one row in the translated markup is bigger than its neighbours purely
  // because that is the frame it was authored in. With this set, that odd row
  // is taken as the Selected template and every row gets the template its state
  // calls for, which is what makes the highlight grow the way it does in game.
  bool state_frames = false;

 private:
  // The rows are looked up by name each time rather than held: a widget handle
  // carries a generation, and one rebuilt from a bare index does not resolve.
  base::String holder_name_;
  base::String list_name_;
  base::Vector<Entry> entries_;
  base::Vector<u32> row_ids_;  // widget indices, in list order, for click tests
  mem_size selected_ = 0;
  f32 list_top_ = 0;
  f32 selected_top_ = 0;
  f32 selected_height_ = 0;
};

}  // namespace rx::ui

#endif  // RECREATION_RUNTIME_UI_VANILLA_LIST_H_
