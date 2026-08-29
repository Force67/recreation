#include "runtime/ui/vanilla_start_menu.h"

#include <base/memory/move.h>

#if defined(RECREATION_HAS_UGUI)

#include <ugui/ultragui.h>
#include <ugui/widgets/widget.h>

namespace rx::ui {
namespace {

ugui::wid ChildNamed(ugui::WidgetRegistry& world, ugui::wid parent, const char* name) {
  const ugui::Hierarchy* h = world.Get<ugui::Hierarchy>(parent);
  if (!h)
    return ugui::kNullWidget;
  for (ugui::wid child : h->children) {
    const ugui::WidgetNode* node = world.Get<ugui::WidgetNode>(child);
    if (node && node->name == name)
      return child;
  }
  return ugui::kNullWidget;
}

void ShowAt(ugui::WidgetRegistry& world, ugui::wid w, f32 top) {
  ugui::StyleC* sc = world.Get<ugui::StyleC>(w);
  if (!sc)
    return;
  ugui::Style style = sc->style;
  style.top = ugui::Length::Px(top);
  style.opacity = 1.0f;
  ugui::SetStyle(world, w, style);
}

}  // namespace

bool VanillaStartMenu::Build(
    ugui::UIContext& ui,
    const Availability& availability,
    const base::UnorderedMap<base::String, base::String>* strings) {
  if (!list_.Bind(ui, "MainListHolder"))
    return false;

  base::Vector<VanillaList::Entry> entries;
  actions_.clear();
  auto add = [&](Action action, const char* key, bool disabled) {
    VanillaList::Entry entry;
    entry.disabled = disabled;
    const base::String* hit = strings ? strings->find(base::String(key)) : nullptr;
    // Without a string table the key itself is the least misleading thing to
    // show; it is what the movie carries.
    entry.text = hit ? *hit : base::String(key + 1);
    entries.push_back(base::move(entry));
    actions_.push_back(action);
  };

  // The same order StartMenu::setupMainMenu pushes.
  if (availability.has_save)
    add(Action::kContinue, "$CONTINUE", false);
  add(Action::kNew, "$NEW", false);
  add(Action::kLoad, "$LOAD", !availability.has_save);
  if (availability.has_creations)
    add(Action::kCreations, "$CREATIONS", false);
  if (availability.has_mods)
    add(Action::kMods, "$MOD MANAGER", false);
  add(Action::kCredits, "$CREDITS", false);
  if (availability.can_quit)
    add(Action::kQuit, "$QUIT", false);

  list_.SetEntries(base::move(entries));
  return true;
}

void VanillaStartMenu::Apply(ugui::UIContext& ui) {
  list_.Apply(ui);
  if (list_.empty())
    return;

  // The arrow keeps its authored x and rides the selected row.
  ugui::WidgetRegistry& world = ui.world();
  const ugui::wid holder = ui.FindWidget("MainListHolder");
  const ugui::wid arrow = holder.valid() ? ChildNamed(world, holder, "SelectionArrow")
                                         : ugui::kNullWidget;
  if (!arrow.valid())
    return;
  const ugui::StyleC* sc = world.Get<ugui::StyleC>(arrow);
  const f32 arrow_height = sc ? sc->style.height.value : 0.0f;
  ShowAt(world, arrow,
         list_.list_top() + list_.selected_top() +
             (list_.selected_height() - arrow_height) * 0.5f);
  const ugui::Hierarchy* h = world.Get<ugui::Hierarchy>(arrow);
  if (h) {
    for (ugui::wid child : h->children) {
      ugui::StyleC* child_style = world.Get<ugui::StyleC>(child);
      if (!child_style)
        continue;
      ugui::Style style = child_style->style;
      style.opacity = 1.0f;
      ugui::SetStyle(world, child, style);
    }
  }
}

void VanillaStartMenu::MoveSelection(int delta) {
  list_.MoveSelection(delta);
}

bool VanillaStartMenu::HandleClick(ugui::UIContext& ui, u32 target_id) {
  return list_.HandleClick(ui, target_id);
}

VanillaStartMenu::Action VanillaStartMenu::Selected() const {
  if (list_.selected() >= actions_.size())
    return Action::kNone;
  return actions_[list_.selected()];
}

}  // namespace rx::ui

#else  // RECREATION_HAS_UGUI

namespace rx::ui {

bool VanillaStartMenu::Build(ugui::UIContext&,
                             const Availability&,
                             const base::UnorderedMap<base::String, base::String>*) {
  return false;
}
void VanillaStartMenu::Apply(ugui::UIContext&) {}
void VanillaStartMenu::MoveSelection(int) {}
bool VanillaStartMenu::HandleClick(ugui::UIContext&, u32) {
  return false;
}
VanillaStartMenu::Action VanillaStartMenu::Selected() const {
  return Action::kNone;
}

}  // namespace rx::ui

#endif  // RECREATION_HAS_UGUI
