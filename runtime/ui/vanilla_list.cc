#include "runtime/ui/vanilla_list.h"

#include <base/memory/move.h>

#if defined(RECREATION_HAS_UGUI)

#include <ugui/ultragui.h>
#include <ugui/widgets/text.h>
#include <ugui/widgets/widget.h>

namespace rx::ui {
namespace {

// The movies author their rows transparent and the game colours them: the
// highlighted entry white, the rest grey, an unavailable one dimmer still.
const ugui::Color kSelected = ugui::Color::FromHex(0xffffff);
const ugui::Color kUnselected = ugui::Color::FromHex(0xb4b4b4);
const ugui::Color kDisabled = ugui::Color::FromHex(0x6e6e6e);

// Matches `name` or the exporter's deduped form of it, "name_2", "name_3", ...
// Every list component in a movie is called List_mc, so all but the first carry
// a suffix by the time they are one flat tree.
bool NameMatches(base::StringRef want, base::StringRef actual) {
  if (want == actual)
    return true;
  if (actual.size() <= want.size() + 1)
    return false;
  for (mem_size i = 0; i < want.size(); ++i)
    if (actual[i] != want[i])
      return false;
  if (actual[want.size()] != '_')
    return false;
  for (mem_size i = want.size() + 1; i < actual.size(); ++i)
    if (actual[i] < '0' || actual[i] > '9')
      return false;
  return true;
}

ugui::wid ChildNamed(ugui::WidgetRegistry& world, ugui::wid parent, base::StringRef name) {
  const ugui::Hierarchy* h = world.Get<ugui::Hierarchy>(parent);
  if (!h)
    return ugui::kNullWidget;
  for (ugui::wid child : h->children) {
    const ugui::WidgetNode* node = world.Get<ugui::WidgetNode>(child);
    if (node && NameMatches(name, base::StringRef(node->name.c_str())))
      return child;
  }
  return ugui::kNullWidget;
}

// The first text widget anywhere under `root`; a row wraps its label in a
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

// Whether `id` is `root` or anything below it.
bool Contains(ugui::WidgetRegistry& world, ugui::wid root, u32 id) {
  if (root.index == id)
    return true;
  const ugui::Hierarchy* h = world.Get<ugui::Hierarchy>(root);
  if (!h)
    return false;
  for (ugui::wid child : h->children) {
    if (Contains(world, child, id))
      return true;
  }
  return false;
}

const ugui::Style* StyleOf(ugui::WidgetRegistry& world, ugui::wid w) {
  const ugui::StyleC* sc = world.Get<ugui::StyleC>(w);
  return sc ? &sc->style : nullptr;
}

void SetTop(ugui::WidgetRegistry& world, ugui::wid w, f32 top) {
  const ugui::Style* current = StyleOf(world, w);
  if (!current)
    return;
  ugui::Style style = *current;
  style.top = ugui::Length::Px(top);
  ugui::SetStyle(world, w, style);
}

void SetHeight(ugui::WidgetRegistry& world, ugui::wid w, f32 height) {
  const ugui::Style* current = StyleOf(world, w);
  if (!current)
    return;
  ugui::Style style = *current;
  style.height = ugui::Length::Px(height);
  ugui::SetStyle(world, w, style);
}

void SetOpacity(ugui::WidgetRegistry& world, ugui::wid w, f32 opacity) {
  const ugui::Style* current = StyleOf(world, w);
  if (!current)
    return;
  ugui::Style style = *current;
  style.opacity = opacity;
  ugui::SetStyle(world, w, style);
}

// One of the component's entry frames, read off a row the exporter captured in
// that frame: how tall the clip is and where its label sits inside it.
struct RowFrame {
  f32 height = 0;
  f32 label_top = 0;
  f32 label_height = 0;
  f32 font_size = 0;
};

RowFrame ReadFrame(ugui::WidgetRegistry& world, ugui::wid row) {
  RowFrame frame;
  const ugui::Style* row_style = StyleOf(world, row);
  if (row_style)
    frame.height = row_style->height.value;
  const ugui::Style* label = StyleOf(world, FirstText(world, row));
  if (label) {
    frame.label_top = label->top.value;
    frame.label_height = label->height.value;
    frame.font_size = label->font_size;
  }
  return frame;
}

void WearFrame(ugui::WidgetRegistry& world, ugui::wid row, const RowFrame& frame) {
  if (frame.height > 0)
    SetHeight(world, row, frame.height);
  const ugui::wid label = FirstText(world, row);
  const ugui::Style* current = StyleOf(world, label);
  if (!current || frame.font_size <= 0)
    return;
  ugui::Style style = *current;
  style.top = ugui::Length::Px(frame.label_top);
  style.height = ugui::Length::Px(frame.label_height);
  style.font_size = frame.font_size;
  ugui::SetStyle(world, label, style);
}

void SetTextColor(ugui::WidgetRegistry& world, ugui::wid w, ugui::Color color) {
  const ugui::Style* current = StyleOf(world, w);
  if (!current)
    return;
  ugui::Style style = *current;
  style.text_color = color;
  ugui::SetStyle(world, w, style);
}

// The rows in list order: the list's children that carry a label, which leaves
// out its `border` spacer.
base::Vector<ugui::wid> Rows(ugui::UIContext& ui,
                             base::StringRef holder,
                             base::StringRef list,
                             ugui::wid* list_out) {
  base::Vector<ugui::wid> rows;
  ugui::WidgetRegistry& world = ui.world();
  const ugui::wid holder_widget = ui.FindWidget(base::String(holder).c_str());
  if (!holder_widget.valid())
    return rows;
  const ugui::wid list_widget = ChildNamed(world, holder_widget, list);
  if (!list_widget.valid())
    return rows;
  if (list_out)
    *list_out = list_widget;
  const ugui::Hierarchy* h = world.Get<ugui::Hierarchy>(list_widget);
  if (!h)
    return rows;
  for (ugui::wid child : h->children) {
    if (FirstText(world, child).valid())
      rows.push_back(child);
  }
  return rows;
}

}  // namespace

bool VanillaList::Bind(ugui::UIContext& ui, base::StringRef holder, base::StringRef list) {
  holder_name_ = base::String(holder);
  list_name_ = base::String(list);
  row_ids_.clear();

  ugui::wid list_widget = ugui::kNullWidget;
  const base::Vector<ugui::wid> rows = Rows(ui, holder_name_, list_name_, &list_widget);
  if (rows.empty()) {
    holder_name_ = base::String();
    return false;
  }
  const ugui::Style* list_style = StyleOf(ui.world(), list_widget);
  list_top_ = list_style ? list_style->top.value : 0.0f;
  for (ugui::wid row : rows)
    row_ids_.push_back(row.index);
  return true;
}

void VanillaList::SetEntries(base::Vector<Entry> entries) {
  entries_ = base::move(entries);
  if (selected_ >= entries_.size())
    selected_ = 0;
  // Never rest on an entry the player cannot pick.
  for (mem_size i = 0; i < entries_.size() && entries_[selected_].disabled; ++i)
    selected_ = (selected_ + 1) % entries_.size();
}

void VanillaList::Apply(ugui::UIContext& ui) {
  const base::Vector<ugui::wid> rows = Rows(ui, holder_name_, list_name_, nullptr);
  if (rows.empty())
    return;
  ugui::WidgetRegistry& world = ui.world();

  // The list stacks from the first row's authored position, each row below the
  // previous by its own height (Shared.BSScrollingList::UpdateList).
  const ugui::Style* first_style = StyleOf(world, rows[0]);
  f32 cursor = first_style ? first_style->top.value : 0.0f;

  const RowFrame normal = ReadFrame(world, rows[0]);
  RowFrame selected = normal;
  if (state_frames) {
    // The one row the exporter caught in the Selected frame is the odd-sized
    // one; the component's clips are otherwise identical.
    for (mem_size i = 1; i < rows.size(); ++i) {
      const RowFrame frame = ReadFrame(world, rows[i]);
      if (frame.height > normal.height + 0.5f) {
        selected = frame;
        break;
      }
    }
  }

  selected_top_ = cursor;
  selected_height_ = normal.height;

  for (mem_size i = 0; i < rows.size(); ++i) {
    const bool used = i < entries_.size();
    const ugui::wid label = FirstText(world, rows[i]);
    if (!used) {
      // The component hides its spare clips. Opacity does not inherit here, so
      // the label is the one that has to go.
      SetOpacity(world, rows[i], 0.0f);
      SetOpacity(world, label, 0.0f);
      continue;
    }

    const RowFrame& frame = (state_frames && i == selected_) ? selected : normal;
    SetOpacity(world, rows[i], 1.0f);
    SetTop(world, rows[i], cursor);
    if (state_frames)
      WearFrame(world, rows[i], frame);

    if (label.valid()) {
      ugui::SetText(label, entries_[i].text.c_str());
      SetOpacity(world, label, 1.0f);
      SetTextColor(world, label,
                   entries_[i].disabled ? kDisabled
                   : i == selected_     ? kSelected
                                        : kUnselected);
    }

    const ugui::Style* style = StyleOf(world, rows[i]);
    const f32 height = state_frames ? frame.height : (style ? style->height.value : normal.height);
    if (i == selected_) {
      selected_top_ = cursor;
      selected_height_ = height;
    }
    cursor += height;
  }
}

void VanillaList::MoveSelection(int delta) {
  if (entries_.empty() || delta == 0)
    return;
  const mem_size count = entries_.size();
  mem_size next = selected_;
  for (mem_size step = 0; step < count; ++step) {
    next = static_cast<mem_size>((static_cast<i64>(next) + delta + count) % count);
    if (!entries_[next].disabled) {
      selected_ = next;
      return;
    }
  }
}

bool VanillaList::HandleClick(ugui::UIContext& ui, u32 target_id) {
  if (entries_.empty())
    return false;
  // The deepest widget under the pointer is the label, not the row, so each row
  // is searched downwards. Walking up from the hit instead would mean rebuilding
  // a handle from its bare index, which does not resolve.
  const base::Vector<ugui::wid> rows = Rows(ui, holder_name_, list_name_, nullptr);
  ugui::WidgetRegistry& world = ui.world();
  for (mem_size i = 0; i < rows.size() && i < entries_.size(); ++i) {
    if (entries_[i].disabled)
      continue;
    if (!Contains(world, rows[i], target_id))
      continue;
    selected_ = i;
    return true;
  }
  return false;
}

}  // namespace rx::ui

#else  // RECREATION_HAS_UGUI

namespace rx::ui {

bool VanillaList::Bind(ugui::UIContext&, base::StringRef, base::StringRef) {
  return false;
}
void VanillaList::SetEntries(base::Vector<Entry> entries) {
  entries_ = base::move(entries);
}
void VanillaList::Apply(ugui::UIContext&) {}
void VanillaList::MoveSelection(int) {}
bool VanillaList::HandleClick(ugui::UIContext&, u32) {
  return false;
}

}  // namespace rx::ui

#endif  // RECREATION_HAS_UGUI
