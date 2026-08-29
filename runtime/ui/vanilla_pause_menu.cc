#include "runtime/ui/vanilla_pause_menu.h"

#include <base/memory/move.h>

#if defined(RECREATION_HAS_UGUI)

#include <base/algorithm.h>
#include <base/strings/format.h>
#include <base/strings/to_string.h>
#include <ugui/ultragui.h>
#include <ugui/widgets/widget.h>

#include <cmath>

#include "runtime/ui/vanilla_ui.h"

namespace rx::ui {
namespace {

// The tab the page belongs to is white, the other two sit back. Both colours
// are the movie's own: it carries a white and a #666666 copy of the tab label.
constexpr u32 kTabActive = 0xffffff;
constexpr u32 kTabIdle = 0x666666;

// Everything on the System page that belongs to a state other than MAIN_STATE.
// The movie stacks them all at their own coordinates in the single frame a
// static translation captures, so they draw on top of the category list.
const char* const kOtherStatePanels[] = {
    "ConfirmPanel",   "SettingsPanel",   "ControlsPanel",     "OptionsListsPanel",
    "InputMappingPanel", "PCQuitPanel",  "SaveLoadPanel",     "ErrorText",
    "HelpListPanel",  "HelpTextPanel",   "CreationListPanel", "CreationTextPanel",
    "PanelRect",
};

// MAIN_STATE clears the bottom bar: the Delete / Character Selection buttons
// the System tab defines only come up in the save/load state.
const char* const kBottomBarButtons[] = {
    "Button1", "Button2", "Button3", "MouseButton1", "MouseButton2", "MouseButton3",
};

// The journal's bottom bar reads "1:23pm, 17th of Last Seed, 4E 201". The
// Tamrielic calendar keeps our month lengths and renames the months, and
// Skyrim starts on the 17th of Last Seed, 4E 201, so the clock's day count
// counts forward from there.
base::String TamrielDate(f64 game_days) {
  static const char* const kMonths[12] = {
      "Morning Star", "Sun's Dawn",  "First Seed", "Rain's Hand",
      "Second Seed",  "Mid Year",    "Sun's Height", "Last Seed",
      "Hearthfire",   "Frostfall",   "Sun's Dusk", "Evening Star"};
  static const int kMonthDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  constexpr int kStartMonth = 7;  // Last Seed
  constexpr int kStartDay = 17;
  constexpr int kStartYear = 201;

  const f64 whole = std::floor(game_days);
  const f64 fraction = game_days - whole;
  int hour = static_cast<int>(fraction * 24.0);
  int minute = static_cast<int>((fraction * 24.0 - hour) * 60.0);
  if (minute > 59)
    minute = 59;
  const char* meridiem = hour < 12 ? "am" : "pm";
  int display_hour = hour % 12;
  if (display_hour == 0)
    display_hour = 12;

  int month = kStartMonth;
  int year = kStartYear;
  i64 day = kStartDay + static_cast<i64>(whole);
  while (day > kMonthDays[month]) {
    day -= kMonthDays[month];
    if (++month == 12) {
      month = 0;
      ++year;
    }
  }

  // 1st, 2nd, 3rd, then th, with the usual exception in the teens.
  const i64 tens = day % 100;
  const char* suffix = "th";
  if (tens < 11 || tens > 13) {
    if (day % 10 == 1)
      suffix = "st";
    else if (day % 10 == 2)
      suffix = "nd";
    else if (day % 10 == 3)
      suffix = "rd";
  }
  return base::Format("{}:{:02}{}, {}{} of {}, 4E {}", display_hour, minute, meridiem, day,
                      suffix, kMonths[month], year);
}

base::String Translate(const base::UnorderedMap<base::String, base::String>* strings,
                       const char* key) {
  const base::String* hit = strings ? strings->find(base::String(key)) : nullptr;
  // Without a string table the key itself is the least misleading thing to
  // show; it is what the movie carries.
  return hit ? *hit : base::String(key + 1);
}

}  // namespace

bool VanillaPauseMenu::Build(
    ugui::UIContext& ui,
    const Availability& availability,
    const base::UnorderedMap<base::String, base::String>* strings) {
  // The list's clips were captured in two different component frames, one of
  // them the taller highlight; see VanillaList::state_frames.
  list_.state_frames = true;
  if (!list_.Bind(ui, "CategoryList_mc"))
    return false;

  base::Vector<VanillaList::Entry> entries;
  actions_.clear();
  auto add = [&](Action action, const char* key) {
    VanillaList::Entry entry;
    entry.text = Translate(strings, key);
    entries.push_back(base::move(entry));
    actions_.push_back(action);
  };

  // The same order SystemPage::onLoad pushes, with $CREATIONS spliced in at
  // MOD_MANAGER_BUTTON_INDEX when the build has it (SystemPage::SetShowMod).
  add(Action::kQuickSave, "$QUICKSAVE");
  add(Action::kSave, "$SAVE");
  add(Action::kLoad, "$LOAD");
  add(Action::kInstalledContent, "$INSTALLED CONTENT");
  if (availability.has_creations)
    add(Action::kCreations, "$CREATIONS");
  add(Action::kSettings, "$SETTINGS");
  add(Action::kControls, "$CONTROLS");
  add(Action::kHelp, "$HELP");
  add(Action::kQuit, "$QUIT");
  list_.SetEntries(base::move(entries));

  // The tabs carry "BUTTON TEXT" until the game labels them. There is no
  // $STATS key in the interface table, so that one is the literal word.
  const base::String quest_tab = Translate(strings, "$QUEST");
  const base::String system_tab = Translate(strings, "$SYSTEM");
  SetVanillaText(ui, "QuestsTab", quest_tab);
  SetVanillaText(ui, "StatsTab", "STATS");
  SetVanillaText(ui, "SystemTab", system_tab);
  SetVanillaTextColor(ui, "QuestsTab", kTabIdle);
  SetVanillaTextColor(ui, "StatsTab", kTabIdle);
  SetVanillaTextColor(ui, "SystemTab", kTabActive);

  // ESC opens the journal with the System page in front; the other two pages
  // fade out (Quest_Journal::SwitchPageToFront).
  ShowVanillaSubtree(ui, "QuestsFader", false);
  ShowVanillaSubtree(ui, "StatsFader", false);
  // The divider's art is faded out in the frame a static translation catches;
  // the page plays it to "Right" as it opens.
  ShowVanillaSubtree(ui, "SystemDivider", true);
  for (const char* panel : kOtherStatePanels)
    ShowVanillaSubtree(ui, panel, false);
  for (const char* button : kBottomBarButtons)
    ShowVanillaSubtree(ui, button, false);
  return true;
}

void VanillaPauseMenu::Apply(ugui::UIContext& ui) {
  list_.Apply(ui);
}

void VanillaPauseMenu::MoveSelection(int delta) {
  list_.MoveSelection(delta);
}

bool VanillaPauseMenu::HandleClick(ugui::UIContext& ui, u32 target_id) {
  return list_.HandleClick(ui, target_id);
}

VanillaPauseMenu::Action VanillaPauseMenu::Selected() const {
  if (list_.selected() >= actions_.size())
    return Action::kNone;
  return actions_[list_.selected()];
}

void VanillaPauseMenu::SetPlayerInfo(ugui::UIContext& ui,
                                     f64 game_days,
                                     int level,
                                     f32 progress) {
  SetVanillaText(ui, "DateText", TamrielDate(game_days));
  SetVanillaText(ui, "LevelNumberLabel", base::ToString(level));
  // The meter is a frame-per-percent clip in the movie, which a static
  // translation cannot step; clipping the fill's mask does the same job.
  ugui::WidgetRegistry& world = ui.world();
  const ugui::wid mask = ui.FindWidget("mask6");
  ugui::StyleC* sc = mask.valid() ? world.Get<ugui::StyleC>(mask) : nullptr;
  if (!sc)
    return;
  if (meter_width_ < 0.0f)
    meter_width_ = sc->style.width.value;
  ugui::Style style = sc->style;
  style.width = ugui::Length::Px(meter_width_ * base::Min(1.0f, base::Max(0.0f, progress)));
  ugui::SetStyle(world, mask, style);
}

}  // namespace rx::ui

#else  // RECREATION_HAS_UGUI

namespace rx::ui {

bool VanillaPauseMenu::Build(ugui::UIContext&,
                             const Availability&,
                             const base::UnorderedMap<base::String, base::String>*) {
  return false;
}
void VanillaPauseMenu::Apply(ugui::UIContext&) {}
void VanillaPauseMenu::MoveSelection(int) {}
bool VanillaPauseMenu::HandleClick(ugui::UIContext&, u32) {
  return false;
}
VanillaPauseMenu::Action VanillaPauseMenu::Selected() const {
  return Action::kNone;
}
void VanillaPauseMenu::SetPlayerInfo(ugui::UIContext&, base::StringRef, base::StringRef, f32) {}

}  // namespace rx::ui

#endif  // RECREATION_HAS_UGUI
