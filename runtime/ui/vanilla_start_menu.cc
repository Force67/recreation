#include "runtime/ui/vanilla_start_menu.h"

#if defined(RECREATION_HAS_UGUI)

#include <base/memory/move.h>
#include <ugui/ultragui.h>
#include <ugui/widgets/text.h>
#include <ugui/widgets/widget.h>

namespace rx::ui {
namespace {

// The movie authors its rows transparent and the game colours them: the
// centred entry white, the rest grey, an unavailable one dimmer still.
const ugui::Color kSelected = ugui::Color::FromHex(0xffffff);
const ugui::Color kUnselected = ugui::Color::FromHex(0xb4b4b4);
const ugui::Color kDisabled = ugui::Color::FromHex(0x6e6e6e);

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

// The first text widget anywhere under `root`; a list row wraps its label in a
// spacer sprite, so the label is not always a direct child.
ugui::wid FirstText(ugui::WidgetRegistry& world, ugui::wid root) {
  const ugui::WidgetNode* node = world.Get<ugui::WidgetNode>(root);
  if (node && node->kind == ugui::WidgetKind::kText)
    return root;
  const ugui::Hierarchy* h = world.Get<ugui::Hierarchy>(root);
  if (!h)
    return ugui::kNullWidget;
  for (ugui::wid child : h->children) {
    const ugui::wid found = FirstText(world, child);
    if (found.valid())
      return found;
  }
  return ugui::kNullWidget;
}

// The option rows of a list: its children that carry a label. The list also
// holds a `border` spacer, which has none.
base::Vector<ugui::wid> ListRows(ugui::WidgetRegistry& world, ugui::wid list) {
  base::Vector<ugui::wid> rows;
  const ugui::Hierarchy* h = world.Get<ugui::Hierarchy>(list);
  if (!h)
    return rows;
  for (ugui::wid child : h->children) {
    if (FirstText(world, child).valid())
      rows.push_back(child);
  }
  return rows;
}

f32 StyleHeight(ugui::WidgetRegistry& world, ugui::wid w) {
  const ugui::StyleC* sc = world.Get<ugui::StyleC>(w);
  return sc ? sc->style.height.value : 0.0f;
}

void SetTop(ugui::WidgetRegistry& world, ugui::wid w, f32 top) {
  ugui::StyleC* sc = world.Get<ugui::StyleC>(w);
  if (!sc)
    return;
  ugui::Style style = sc->style;
  style.top = ugui::Length::Px(top);
  ugui::SetStyle(world, w, style);
}

void SetOpacity(ugui::WidgetRegistry& world, ugui::wid w, f32 opacity) {
  ugui::StyleC* sc = world.Get<ugui::StyleC>(w);
  if (!sc)
    return;
  ugui::Style style = sc->style;
  style.opacity = opacity;
  ugui::SetStyle(world, w, style);
}

void SetTextColor(ugui::WidgetRegistry& world, ugui::wid w, ugui::Color color) {
  ugui::StyleC* sc = world.Get<ugui::StyleC>(w);
  if (!sc)
    return;
  ugui::Style style = sc->style;
  style.text_color = color;
  ugui::SetStyle(world, w, style);
}

}  // namespace

void VanillaStartMenu::Build(
    const Availability& availability,
    const base::UnorderedMap<base::String, base::String>* strings) {
  options_.clear();
  auto add = [&](Action action, const char* key, bool disabled) {
    Option option;
    option.action = action;
    option.disabled = disabled;
    const base::String lookup(key);
    const base::String* hit = strings ? strings->find(lookup) : nullptr;
    // Without a string table the key itself is the least misleading thing to
    // show; it is what the movie carries.
    option.text = hit ? *hit : base::String(key + 1);
    options_.push_back(base::move(option));
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

  if (selected_ >= options_.size())
    selected_ = 0;
  // Never rest on an entry the player cannot pick.
  for (mem_size i = 0; i < options_.size() && options_[selected_].disabled; ++i)
    selected_ = (selected_ + 1) % options_.size();
}

void VanillaStartMenu::Apply(ugui::UIContext& ui) {
  if (options_.empty())
    return;
  ugui::WidgetRegistry& world = ui.world();
  const ugui::wid holder = ui.FindWidget("MainListHolder");
  if (!holder.valid())
    return;
  const ugui::wid list = ChildNamed(world, holder, "List_mc");
  if (!list.valid())
    return;

  base::Vector<ugui::wid> rows = ListRows(world, list);
  if (rows.empty())
    return;

  // The list stacks from the first row's authored position, each row below the
  // previous one by its own height (Shared.CenteredScrollingList::UpdateList).
  const ugui::StyleC* first = world.Get<ugui::StyleC>(rows[0]);
  f32 cursor = first ? first->style.top.value : 0.0f;
  f32 selected_top = cursor;
  f32 selected_height = 0;

  for (mem_size i = 0; i < rows.size(); ++i) {
    const bool used = i < options_.size();
    SetOpacity(world, rows[i], used ? 1.0f : 0.0f);
    if (!used)
      continue;

    Option& option = options_[i];
    option.row_id = rows[i].index;
    SetTop(world, rows[i], cursor);

    const ugui::wid label = FirstText(world, rows[i]);
    if (label.valid()) {
      ugui::SetText(label, option.text.c_str());
      SetTextColor(world, label,
                   option.disabled  ? kDisabled
                   : i == selected_ ? kSelected
                                    : kUnselected);
    }
    if (i == selected_) {
      selected_top = cursor;
      selected_height = StyleHeight(world, rows[i]);
    }
    cursor += StyleHeight(world, rows[i]);
  }

  // The arrow keeps its authored x and rides the selected row.
  const ugui::wid arrow = ChildNamed(world, holder, "SelectionArrow");
  if (arrow.valid()) {
    const f32 arrow_height = StyleHeight(world, arrow);
    const ugui::StyleC* list_style = world.Get<ugui::StyleC>(list);
    const f32 list_top = list_style ? list_style->style.top.value : 0.0f;
    SetTop(world, arrow,
           list_top + selected_top + (selected_height - arrow_height) * 0.5f);
    SetOpacity(world, arrow, 1.0f);
    const ugui::Hierarchy* h = world.Get<ugui::Hierarchy>(arrow);
    if (h) {
      for (ugui::wid child : h->children)
        SetOpacity(world, child, 1.0f);
    }
  }
}

void VanillaStartMenu::MoveSelection(int delta) {
  if (options_.empty() || delta == 0)
    return;
  const mem_size count = options_.size();
  mem_size next = selected_;
  for (mem_size step = 0; step < count; ++step) {
    next = static_cast<mem_size>((static_cast<i64>(next) + delta + count) % count);
    if (!options_[next].disabled) {
      selected_ = next;
      return;
    }
  }
}

bool VanillaStartMenu::HandleClick(ugui::UIContext& ui, u32 target_id) {
  if (options_.empty())
    return false;
  ugui::WidgetRegistry& world = ui.world();
  // The deepest widget under the pointer is the label, so walk up to the row.
  ugui::wid current{target_id, 0};
  for (int depth = 0; depth < 8 && current.index != 0; ++depth) {
    for (mem_size i = 0; i < options_.size(); ++i) {
      if (options_[i].row_id != current.index || options_[i].disabled)
        continue;
      selected_ = i;
      return true;
    }
    const ugui::Hierarchy* h = world.Get<ugui::Hierarchy>(current);
    if (!h)
      return false;
    current = h->parent;
  }
  return false;
}

VanillaStartMenu::Action VanillaStartMenu::Selected() const {
  if (options_.empty())
    return Action::kNone;
  return options_[selected_].action;
}

}  // namespace rx::ui

#else  // RECREATION_HAS_UGUI

namespace rx::ui {

void VanillaStartMenu::Build(const Availability&,
                             const base::UnorderedMap<base::String, base::String>*) {}
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
