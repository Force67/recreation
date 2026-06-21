#include "game_ui.h"

#include "fly_camera.h"

#if defined(RECREATION_HAS_UGUI)

#include <ugui/core/color.h>
#include <ugui/style/style.h>
#include <ugui/ultragui.h>
#include <ugui/widgets/text.h>
#include <ugui/widgets/widget.h>
#include <ugui/widgets/widget_registry.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "core/log.h"
#include "core/window.h"
#include "gui_backend.h"
#include "render/renderer.h"
#include "ugui_platform.h"

namespace rec {
namespace {

namespace fs = std::filesystem;

// Scrolling compass geometry. 8 marks per 360deg turn, 3 turns so the strip
// always covers the window whatever the heading; the engine slides it by
// setting compass_strip's left offset each frame.
constexpr float kCompassWindow = 340.0f;
constexpr float kCompassLabel = 70.0f;
constexpr int kCompassTurns = 3;
constexpr float kCompassCenter = kCompassWindow * 0.5f;

const char* kCardinals[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};

float CompassStripLeft(float heading_deg) {
  float eff = heading_deg + 360.0f;  // middle turn
  return kCompassCenter - (eff / 45.0f) * kCompassLabel - kCompassLabel * 0.5f;
}

// The tracked quest never shows more than a handful of objectives at once, so a
// fixed pool of pre-declared rows is filled and toggled each frame; the static
// ultragui document has no way to add widgets on the fly.
constexpr int kQuestObjectiveRows = 6;
constexpr int kDialogueOptionRows = 4;  // matches the 1-4 selection keys
constexpr int kJournalRows = 6;         // quests listed in the journal (1-N pick, 1-4 usable)
constexpr int kJournalObjRows = 6;      // objectives shown for the selected journal quest
constexpr int kContainerRows = 14;      // item rows in the container loot panel
constexpr float kToastSeconds = 4.0f;

// The quest tracker (top-right objective list), the "quest updated" banner, and
// the centered activation prompt. Their text and visibility are driven each
// frame from engine state; declared empty so they start hidden.
std::string BuildQuestSection() {
  std::string s = R"(
  panel questtracker {
    position: absolute; right: 30; top: 86; layout: column; align: end; gap: 5; width: 380;
    text quest_title { text: ""; font-size: 17; color: #ffcc55; text-align: right;
      letter-spacing: 1; text-shadow-color: #000000d0; text-shadow-x: 1; text-shadow-y: 1; }
)";
  for (int i = 0; i < kQuestObjectiveRows; ++i) {
    s += "    text quest_obj" + std::to_string(i) +
         " { text: \"\"; font-size: 13; color: #d8def0; text-align: right;"
         " text-shadow-color: #000000c0; text-shadow-x: 1; text-shadow-y: 1; }\n";
  }
  s += R"(  }

  panel quest_marker_box {
    position: absolute; top: 52; left: 0; width: 100vw; layout: column; align: center;
    text quest_marker_text { text: ""; font-size: 13; color: #ffe6a0; letter-spacing: 1;
      text-shadow-color: #000000d0; text-shadow-x: 1; text-shadow-y: 1; }
  }

  panel quest_toast_box {
    position: absolute; top: 120; left: 0; width: 100vw; layout: column; align: center;
    text quest_toast { text: ""; font-size: 21; color: #ffcc55; letter-spacing: 3;
      text-transform: uppercase; text-shadow-color: #000000e0; text-shadow-x: 1; text-shadow-y: 2; }
  }

  panel activate_box {
    position: absolute; top: 0; left: 0; width: 100vw; height: 100vh;
    layout: column; justify: center; align: center;
    text activate_prompt { text: ""; font-size: 18; color: #f0f2fb; margin: 110 0 0 0;
      text-shadow-color: #000000e0; text-shadow-x: 1; text-shadow-y: 1; }
  }
)";
  return s;
}

// The quest journal overlay: a dimmed full-screen panel with a card listing the
// player's active quests and, for the selected one, its objectives. Rows are a
// fixed pool filled and toggled each frame; starts hidden.
std::string BuildJournalSection() {
  std::string s = R"(
  panel journal_box {
    position: absolute; left: 0; top: 0; width: 100vw; height: 100vh;
    layout: column; justify: center; align: center; background: #05070cdd;
    panel journal_card {
      layout: column; align: start; padding: 30 44; width: 580;
      background: #0d1018f7; corner-radius: 14; border-color: #ffffff1c; border-width: 1;
      shadow-color: #000000aa; shadow-blur: 48; shadow-y: 18;
      text journal_head { text: "Journal"; font-size: 24; color: #ffcc55; letter-spacing: 6;
        text-transform: uppercase; margin: 0 0 14 0; }
)";
  for (int i = 0; i < kJournalRows; ++i) {
    s += "      text journal_q" + std::to_string(i) +
         " { text: \"\"; font-size: 16; color: #d8def0; margin: 0 0 3 0;"
         " text-shadow-color: #000000c0; text-shadow-x: 1; text-shadow-y: 1; }\n";
  }
  s += "      panel journal_rule { width: 492; height: 1; background: #ffffff1f; margin: 12 0 12 "
       "0; }\n";
  for (int i = 0; i < kJournalObjRows; ++i) {
    s += "      text journal_obj" + std::to_string(i) +
         " { text: \"\"; font-size: 13; color: #c7e0ff; margin: 0 0 2 0;"
         " text-shadow-color: #000000c0; text-shadow-x: 1; text-shadow-y: 1; }\n";
  }
  s += R"(      text journal_hint { text: "press a number to track, J to close"; font-size: 12;
        color: #8a93a8; margin: 14 0 0 0; }
    }
  }
)";
  return s;
}

// The dialogue panel (bottom-centre): the speaker, their last line, and a fixed
// pool of numbered topic rows filled/toggled each frame.
std::string BuildDialogueSection() {
  std::string s = R"(
  panel dialogue_box {
    position: absolute; left: 0; bottom: 70; width: 100vw; layout: column; align: center; gap: 4;
    text dialogue_speaker { text: ""; font-size: 18; color: #ffcc55;
      text-shadow-color: #000000e0; text-shadow-x: 1; text-shadow-y: 1; }
    text dialogue_npc { text: ""; font-size: 15; color: #d8def0; margin: 0 0 8 0;
      text-shadow-color: #000000c0; text-shadow-x: 1; text-shadow-y: 1; }
)";
  for (int i = 0; i < kDialogueOptionRows; ++i) {
    s += "    text dialogue_opt" + std::to_string(i) +
         " { text: \"\"; font-size: 15; color: #c7e0ff;"
         " text-shadow-color: #000000c0; text-shadow-x: 1; text-shadow-y: 1; }\n";
  }
  s += "  }\n";
  return s;
}

// The container loot panel: a dimmed full-screen overlay with a card naming the
// container and listing its contents in a fixed pool of rows. Starts hidden.
std::string BuildContainerSection() {
  std::string s = R"(
  panel container_box {
    position: absolute; left: 0; top: 0; width: 100vw; height: 100vh;
    layout: column; justify: center; align: center; background: #05070cdd;
    panel container_card {
      layout: column; align: start; padding: 28 40; width: 460;
      background: #0d1018f7; corner-radius: 14; border-color: #ffffff1c; border-width: 1;
      shadow-color: #000000aa; shadow-blur: 48; shadow-y: 18;
      text container_head { text: ""; font-size: 22; color: #ffcc55; letter-spacing: 4;
        text-transform: uppercase; margin: 0 0 12 0; }
)";
  for (int i = 0; i < kContainerRows; ++i) {
    s += "      text container_item" + std::to_string(i) +
         " { text: \"\"; font-size: 15; color: #d8def0; margin: 0 0 2 0;"
         " text-shadow-color: #000000c0; text-shadow-x: 1; text-shadow-y: 1; }\n";
  }
  s += R"(      text container_hint { text: "press Esc to close"; font-size: 12;
        color: #8a93a8; margin: 14 0 0 0; }
    }
  }
)";
  return s;
}

// The map editor overlay: a top toolbar, a left asset-browser dock, a right
// selection inspector, a bottom status bar and a builder reticle. Everything is
// pooled (fixed widget counts filled and toggled each frame) and starts hidden;
// the engine collapses editor_root until the editor is on. Names are matched by
// the click router (btn_tool*, btn_cat*, ed_row*, ed_search*, btn_scroll_*).
std::string BuildEditorSection() {
  std::string s = R"(
  panel editor_root {
    position: absolute; top: 0; left: 0; width: 100vw; height: 100vh;

    panel editor_reticle {
      position: absolute; top: 0; left: 0; width: 100vw; height: 100vh;
      layout: column; justify: center; align: center;
      panel ed_cross {
        width: 24; height: 24; position: relative;
        panel { position: absolute; left: 11; top: 2; width: 2; height: 8; background: #ffcc55dd; }
        panel { position: absolute; left: 11; top: 14; width: 2; height: 8; background: #ffcc55dd; }
        panel { position: absolute; left: 2; top: 11; width: 8; height: 2; background: #ffcc55dd; }
        panel { position: absolute; left: 14; top: 11; width: 8; height: 2; background: #ffcc55dd; }
        panel { position: absolute; left: 10; top: 10; width: 4; height: 4; corner-radius: 2; background: #ffffffee; }
      }
    }

    panel ed_select {
      position: absolute; left: 0; top: 0; width: 64; height: 64;
      panel { position: absolute; left: 0; top: 0; width: 18; height: 3; background: #ffcc55; }
      panel { position: absolute; left: 0; top: 0; width: 3; height: 18; background: #ffcc55; }
      panel { position: absolute; left: 46; top: 0; width: 18; height: 3; background: #ffcc55; }
      panel { position: absolute; left: 61; top: 0; width: 3; height: 18; background: #ffcc55; }
      panel { position: absolute; left: 0; top: 61; width: 18; height: 3; background: #ffcc55; }
      panel { position: absolute; left: 0; top: 46; width: 3; height: 18; background: #ffcc55; }
      panel { position: absolute; left: 46; top: 61; width: 18; height: 3; background: #ffcc55; }
      panel { position: absolute; left: 61; top: 46; width: 3; height: 18; background: #ffcc55; }
    }

    panel editor_toolbar {
      position: absolute; top: 0; left: 0; width: 100vw; height: 46;
      layout: row; align: center; justify: space-between; padding: 0 18;
      background: #0a0c14f7; border-color: #ffffff14; border-width: 1;
      shadow-color: #00000088; shadow-blur: 18; shadow-y: 3;
      panel ed_tb_left { layout: row; align: center; gap: 14;
        text { text: "MAP EDITOR"; font-size: 15; color: #ffcc55; letter-spacing: 3; text-transform: uppercase; }
        text ed_tb_hint { text: ""; font-size: 12; color: #8a93a8; letter-spacing: 1; }
      }
      panel ed_tb_tools { layout: row; align: center; gap: 6;
)";

  // Toolbar action buttons (children of ed_tb_tools). Indices match MapEditor's
  // ToolAction enum.
  const char* tools[kEditorToolButtons] = {"Select", "Move", "Rotate", "Scale",
                                           "Delete", "Dupe", "Undo",   "Save"};
  for (int i = 0; i < kEditorToolButtons; ++i) {
    s += "        button btn_tool" + std::to_string(i) + " { text: \"" + tools[i] +
         "\"; font-size: 13; color: #d8def0; text-align: center; padding: 7 12;"
         " background: #ffffff0e; corner-radius: 7; cursor: pointer;"
         " :hover { background: #ffcc5530; color: #ffffff; }"
         " :pressed { background: #ffcc5555; } }\n";
  }
  s += "      }\n    }\n";  // close ed_tb_tools, editor_toolbar

  // Left asset-browser dock.
  s += R"(
    panel editor_browser {
      position: absolute; left: 0; top: 46; width: 360; bottom: 34;
      layout: column; align: start; padding: 16 18; gap: 10;
      background: #0b0e16f7; border-color: #ffffff12; border-width: 1;
      shadow-color: #000000aa; shadow-blur: 34; shadow-x: 8;
      text { text: "Asset Browser"; font-size: 16; color: #ffcc55; letter-spacing: 3;
        text-transform: uppercase; }
      panel ed_search {
        layout: row; align: center; justify: space-between; width: 324; padding: 9 12;
        background: #05070ccc; corner-radius: 8; border-color: #ffffff16; border-width: 1;
        cursor: text;
        text ed_search_text { text: "Search assets..."; font-size: 14; color: #6b7488; }
        button ed_search_clear { text: "x"; font-size: 13; color: #8a93a8; padding: 0 6;
          background: #ffffff00; cursor: pointer; :hover { color: #ffffff; } }
      }
      panel ed_cats { layout: row; align: center; gap: 6; width: 324;
)";

  // Category tabs (children of ed_cats; pooled, filled/collapsed by the view).
  for (int i = 0; i < kEditorCategoryTabs; ++i) {
    s += "        button btn_cat" + std::to_string(i) +
         " { text: \"\"; font-size: 12; color: #aab2c6; padding: 5 10;"
         " background: #ffffff0c; corner-radius: 6; cursor: pointer;"
         " :hover { background: #ffffff1a; color: #ffffff; } }\n";
  }
  s += "      }\n";  // close ed_cats
  s += "      text ed_result_count { text: \"\"; font-size: 12; color: #8a93a8; }\n";
  s += "      panel ed_rows { layout: column; align: start; gap: 3; width: 324;\n";

  // Asset rows (children of ed_rows; pooled). Each is a clickable panel with a
  // name line and a subtitle line.
  for (int i = 0; i < kEditorBrowserRows; ++i) {
    const std::string id = std::to_string(i);
    s += "        panel ed_row" + id +
         " { layout: column; align: start; width: 304; padding: 7 10; corner-radius: 7;"
         " background: #ffffff08; cursor: pointer; :hover { background: #ffcc5522; }\n"
         "          text ed_row" +
         id +
         "_name { text: \"\"; font-size: 14; color: #e8ecf6; }\n"
         "          text ed_row" +
         id +
         "_sub { text: \"\"; font-size: 11; color: #7e879b; }\n"
         "        }\n";
  }
  s += "      }\n";  // close ed_rows

  s += R"(
      panel ed_scroll {
        layout: row; align: center; gap: 8; width: 324;
        button btn_scroll_up { text: "prev"; font-size: 12; color: #d8def0; padding: 6 12;
          background: #ffffff0e; corner-radius: 6; cursor: pointer;
          :hover { background: #ffcc5530; color: #ffffff; } }
        button btn_scroll_down { text: "next"; font-size: 12; color: #d8def0; padding: 6 12;
          background: #ffffff0e; corner-radius: 6; cursor: pointer;
          :hover { background: #ffcc5530; color: #ffffff; } }
      }
    }

    panel editor_inspector {
      position: absolute; right: 0; top: 46; width: 300; bottom: 34;
      layout: column; align: start; padding: 18 20; gap: 6;
      background: #0b0e16f7; border-color: #ffffff12; border-width: 1;
      shadow-color: #000000aa; shadow-blur: 34; shadow-x: -8;
      text { text: "Inspector"; font-size: 16; color: #ffcc55; letter-spacing: 3;
        text-transform: uppercase; }
      text ed_insp_title { text: ""; font-size: 17; color: #f2f4fb; margin: 6 0 0 0; }
      text ed_insp_sub { text: ""; font-size: 12; color: #8a93a8; margin: 0 0 8 0; }
      panel ed_insp_rule { width: 260; height: 1; background: #ffffff18; margin: 4 0 8 0; }
      text ed_insp_pos { text: ""; font-size: 14; color: #d8def0; }
      text ed_insp_rot { text: ""; font-size: 14; color: #d8def0; }
      text ed_insp_scale { text: ""; font-size: 14; color: #d8def0; }
      panel ed_insp_rule2 { width: 260; height: 1; background: #ffffff18; margin: 10 0 8 0; }
      text { text: "G move   R rotate   wheel scale"; font-size: 12; color: #8a93a8; }
      text { text: "X delete   Ctrl+V dupe   Ctrl+Z undo"; font-size: 12; color: #8a93a8; }
    }

    panel editor_help {
      position: absolute; right: 0; top: 46; width: 300; bottom: 34;
      layout: column; align: start; padding: 18 20; gap: 7;
      background: #0b0e16f7; border-color: #ffffff12; border-width: 1;
      shadow-color: #000000aa; shadow-blur: 34; shadow-x: -8;
      text { text: "Controls"; font-size: 16; color: #ffcc55; letter-spacing: 3;
        text-transform: uppercase; }
      text { text: "Right-drag to fly,  WASD / Q E"; font-size: 13; color: #c7cfdf; margin: 6 0 0 0; }
      panel { width: 260; height: 1; background: #ffffff18; margin: 4 0 4 0; }
      text { text: "1. Click an asset on the left"; font-size: 13; color: #d8def0; }
      text { text: "2. Aim at the ground"; font-size: 13; color: #d8def0; }
      text { text: "3. Click to drop it"; font-size: 13; color: #d8def0; }
      panel { width: 260; height: 1; background: #ffffff18; margin: 4 0 4 0; }
      text { text: "Click a placed object to select"; font-size: 13; color: #c7cfdf; }
      text { text: "Shift+click to multi-select"; font-size: 12; color: #9aa2b6; }
      text { text: "R   rotate the brush / selection"; font-size: 12; color: #9aa2b6; }
      text { text: "G   grab and move,  X delete"; font-size: 12; color: #9aa2b6; }
      text { text: "wheel  scale,   Ctrl+V duplicate"; font-size: 12; color: #9aa2b6; }
      text { text: "B   grid snap,   Ctrl+Z undo"; font-size: 12; color: #9aa2b6; }
      text { text: "F5  save,   F4 exit editor"; font-size: 12; color: #9aa2b6; }
    }

    panel editor_status {
      position: absolute; left: 0; bottom: 0; width: 100vw; height: 34;
      layout: row; align: center; justify: space-between; padding: 0 18;
      background: #0a0c14f7; border-color: #ffffff14; border-width: 1;
      text ed_status_left { text: ""; font-size: 13; color: #ffcc55; }
      text ed_status_right { text: ""; font-size: 12; color: #8a93a8; letter-spacing: 1; }
    }
  }
)";
  return s;
}

std::string BuildUi() {
  std::string s;
  s += R"(
panel root {
  width: 100vw; height: 100vh; position: relative;

  panel topbar {
    position: absolute; top: 0; left: 0; width: 100vw; height: 64;
    layout: row; justify: center; align: start; padding: 16 0 0 0;
    panel compass_window {
      width: 340; height: 30; position: relative; overflow: hidden;
      panel compass_strip {
        position: absolute; top: 0; left: 0; height: 30; width: 1680;
        layout: row; align: center;
)";

  // Generated cardinal labels.
  int count = 8 * kCompassTurns;
  for (int i = 0; i < count; ++i) {
    const char* card = kCardinals[i % 8];
    bool major = (i % 2) == 0;  // N E S W
    bool north = (i % 8) == 0;
    const char* color = north ? "#ffcc55" : (major ? "#e8ecf6" : "#6b7488");
    int font = major ? 15 : 11;
    s += "        text cl" + std::to_string(i) + " { text: \"" + card +
         "\"; width: 70; text-align: center; font-size: " + std::to_string(font) +
         "; color: " + color +
         "; text-shadow-color: #000000d0; text-shadow-x: 1; text-shadow-y: 1; }\n";
  }

  s += R"(
      }
      panel compass_marker {
        position: absolute; top: 0; left: 169; width: 2; height: 30;
        background: #ffcc55;
      }
      panel quest_pip {
        position: absolute; top: 10; left: 0; width: 9; height: 9; corner-radius: 5;
        background: #ffd24a; border-color: #000000aa; border-width: 1; visibility: collapsed;
      }
    }
  }

  panel crosshair {
    position: absolute; top: 0; left: 0; width: 100vw; height: 100vh;
    layout: column; justify: center; align: center;
    panel cross {
      width: 24; height: 24; position: relative;
      panel ch_t { position: absolute; left: 11; top: 0;  width: 2; height: 8; background: #ffffffcc; }
      panel ch_b { position: absolute; left: 11; top: 16; width: 2; height: 8; background: #ffffffcc; }
      panel ch_l { position: absolute; left: 0;  top: 11; width: 8; height: 2; background: #ffffffcc; }
      panel ch_r { position: absolute; left: 16; top: 11; width: 8; height: 2; background: #ffffffcc; }
      panel ch_dot { position: absolute; left: 11; top: 11; width: 2; height: 2; background: #ffcc55; }
    }
  }

  panel vitals {
    position: absolute; left: 30; bottom: 30; layout: column; gap: 9;
    panel bar_health_track {
      width: 280; height: 13; overflow: hidden;
      background: #0c0e16cc; corner-radius: 7;
      border-color: #00000077; border-width: 1;
      shadow-color: #00000066; shadow-blur: 12; shadow-y: 3;
      panel bar_health_fill {
        width: 100%; height: 100%; corner-radius: 7;
        background: #b23a3a; background-end: #e8645d;
      }
    }
    panel bar_magicka_track {
      width: 280; height: 13; overflow: hidden;
      background: #0c0e16cc; corner-radius: 7;
      border-color: #00000077; border-width: 1;
      shadow-color: #00000066; shadow-blur: 12; shadow-y: 3;
      panel bar_magicka_fill {
        width: 100%; height: 100%; corner-radius: 7;
        background: #3a63b2; background-end: #5d92e8;
      }
    }
    panel bar_stamina_track {
      width: 280; height: 13; overflow: hidden;
      background: #0c0e16cc; corner-radius: 7;
      border-color: #00000077; border-width: 1;
      shadow-color: #00000066; shadow-blur: 12; shadow-y: 3;
      panel bar_stamina_fill {
        width: 100%; height: 100%; corner-radius: 7;
        background: #3aa05a; background-end: #5de88a;
      }
    }
  }

  panel readout {
    position: absolute; right: 30; bottom: 30; layout: column; align: end; gap: 3;
    text hud_fps { text: "-- fps"; font-size: 15; color: #cfd6e6;
      text-shadow-color: #000000c0; text-shadow-x: 1; text-shadow-y: 1; }
    text hud_coords { text: "x -- y -- z --"; font-size: 12; color: #aab2c6;
      text-shadow-color: #000000c0; text-shadow-x: 1; text-shadow-y: 1; }
    text hud_heading { text: "--"; font-size: 12; color: #aab2c6;
      text-shadow-color: #000000c0; text-shadow-x: 1; text-shadow-y: 1; }
  }
)";

  s += BuildQuestSection();
  s += BuildJournalSection();
  s += BuildDialogueSection();
  s += BuildContainerSection();
  s += BuildEditorSection();  // before the menu so the pause overlay draws on top

  s += R"(
  panel menu {
    position: absolute; top: 0; left: 0; width: 100vw; height: 100vh;
    layout: column; justify: center; align: center;
    background: #05070cf2;
    panel menu_card {
      layout: column; align: center; padding: 44 60;
      background: #0d1018f7; background-end: #0a0c14f7; corner-radius: 16;
      border-color: #ffffff1c; border-width: 1;
      shadow-color: #000000aa; shadow-blur: 48; shadow-y: 20;
      text menu_title {
        text: "Paused"; font-size: 40; color: #f2f4fb;
        letter-spacing: 10; text-transform: uppercase;
      }
      text menu_sub {
        text: "recreation"; font-size: 13; color: #ffcc55;
        letter-spacing: 4; text-transform: uppercase;
      }
      panel menu_rule { width: 340; height: 1; background: #ffffff1f; margin: 24 0 22 0; }
      panel menu_buttons {
        layout: column; gap: 8; width: 340;
        button btn_resume {
          text: "Resume"; font-size: 19; color: #e8ecf6; text-align: center;
          background: #ffcc5522; corner-radius: 9; padding: 14 0; cursor: pointer;
          border-color: #ffcc5544; border-width: 1;
          :hover { background: #ffcc5538; color: #ffffff; }
          :pressed { background: #ffcc5555; }
        }
        button btn_settings {
          text: "Settings"; font-size: 19; color: #9aa2b6; text-align: center;
          background: #ffffff08; corner-radius: 9; padding: 14 0; cursor: pointer;
          :hover { background: #ffffff14; color: #d8def0; }
          :pressed { background: #ffffff20; }
        }
        button btn_quit {
          text: "Quit to Desktop"; font-size: 19; color: #d88a8a; text-align: center;
          background: #ffffff08; corner-radius: 9; padding: 14 0; cursor: pointer;
          :hover { background: #b23a3a33; color: #ffd0d0; }
          :pressed { background: #b23a3a55; }
        }
      }
      panel menu_settings {
        layout: column; gap: 6; width: 340;
        text settings_head { text: "Controls"; font-size: 14; color: #ffcc55;
          letter-spacing: 3; text-transform: uppercase; margin: 0 0 6 0; }
)";
  // Keybinds list, one row per control.
  const char* keybinds[][2] = {
      {"F1", "Renderer overlay"}, {"F2", "Papyrus natives"}, {"F3", "Quest debugger"},
      {"T", "Toggle walk / fly"}, {"E", "Activate / talk"},  {"J", "Journal"},
      {"Esc", "Pause menu"},
  };
  for (const auto& kb : keybinds) {
    s += "        panel kb_" + std::string(kb[0]) +
         " { layout: row; justify: space-between; align: center; width: 100%; padding: 5 0;\n"
         "          text { text: \"" +
         kb[1] +
         "\"; font-size: 14; color: #d8def0; }\n"
         "          text { text: \"" +
         kb[0] +
         "\"; font-size: 14; color: #9aa2b6; letter-spacing: 1; }\n"
         "        }\n";
  }
  s +=
      R"(        panel settings_rule { width: 340; height: 1; background: #ffffff1f; margin: 16 0 6 0; }
        button btn_settings_back {
          text: "Back"; font-size: 19; color: #e8ecf6; text-align: center;
          background: #ffcc5522; corner-radius: 9; padding: 14 0; cursor: pointer;
          border-color: #ffcc5544; border-width: 1;
          :hover { background: #ffcc5538; color: #ffffff; }
          :pressed { background: #ffcc5555; }
        }
      }
    }
  }
}
)";
  return s;
}

const char* FindFont() {
  static std::string resolved;
  if (const char* env = std::getenv("RECREATION_UI_FONT"); env && fs::exists(env)) {
    resolved = env;
    return resolved.c_str();
  }
  if (FILE* p = popen("fc-match -f '%{file}' 'sans:style=Regular' 2>/dev/null", "r")) {
    char buf[1024];
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, p);
    pclose(p);
    if (n > 0) {
      buf[n] = '\0';
      std::string path(buf);
      while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
      if (!path.empty() && fs::exists(path)) {
        resolved = path;
        return resolved.c_str();
      }
    }
  }
  static const char* candidates[] = {
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/usr/share/fonts/noto/NotoSans-Regular.ttf",
      "/run/current-system/sw/share/X11/fonts/DejaVuSans.ttf",
  };
  for (const char* c : candidates) {
    if (fs::exists(c)) {
      resolved = c;
      return resolved.c_str();
    }
  }
  return nullptr;
}

}  // namespace

struct GameUi::Impl {
  ugui::UIContext ui;
  ui::UguiHostState host;
  ui::GuiRenderBackend backend;
  ugui::FontHandle font = ugui::kInvalidFont;
  u32 font_revision = ~0u;
  const ugui::DrawData* draw_data = nullptr;
  bool initialized = false;
  bool menu_open = false;
  bool settings_open = false;  // settings sub-view of the pause menu
  bool quit_requested = false;
  bool prev_mouse[3] = {};
  float stamina = 1.0f;

  // Quest HUD state, set by the engine and applied each frame.
  HudQuest quest;
  std::string toast_text;
  float toast_age = kToastSeconds + 1.0f;  // starts expired, so hidden
  std::string activate_prompt;
  DialogueView dialogue;
  ContainerView container;
  // Objective compass waypoint, driven by the engine each frame.
  bool marker_active = false;
  float marker_bearing = 0.0f;   // degrees, 0 = ahead, + = right
  float marker_distance = 0.0f;  // meters
  // Quest journal overlay, driven by the engine.
  bool journal_open = false;
  std::vector<HudQuest> journal;
  int journal_selected = -1;

  // Map editor overlay state and the sink that receives its widget clicks.
  EditorView editor;
  std::function<void(const EditorUiEvent&)> editor_sink;
  bool editor_prev_active = false;  // edge-detect to hide/restore the gameplay HUD

  void SetStyleField(const char* name, void (*mutate)(ugui::Style&, float), float arg) {
    ugui::wid w = ui.FindWidget(name);
    if (!w.valid()) return;
    ugui::StyleC* sc = ui.world().Get<ugui::StyleC>(w);
    if (!sc) return;
    ugui::Style s = sc->style;
    mutate(s, arg);
    ugui::SetStyle(ui.world(), w, s);
  }

  void SetVisible(const char* name, bool visible) {
    SetStyleField(
        name,
        [](ugui::Style& s, float v) {
          s.visibility = v > 0.5f ? ugui::Visibility::kVisible : ugui::Visibility::kCollapsed;
        },
        visible ? 1.0f : 0.0f);
  }

  void SetBackground(const char* name, ugui::Color color) {
    ugui::wid w = ui.FindWidget(name);
    if (!w.valid()) return;
    ugui::StyleC* sc = ui.world().Get<ugui::StyleC>(w);
    if (!sc) return;
    ugui::Style style = sc->style;
    style.background = color;
    ugui::SetStyle(ui.world(), w, style);
  }

  void SetTextColor(const char* name, ugui::Color color) {
    ugui::wid w = ui.FindWidget(name);
    if (!w.valid()) return;
    ugui::StyleC* sc = ui.world().Get<ugui::StyleC>(w);
    if (!sc) return;
    ugui::Style style = sc->style;
    style.text_color = color;
    ugui::SetStyle(ui.world(), w, style);
  }

  // Drives every editor widget from the EditorView each frame. Collapses the
  // whole overlay when the editor is off.
  void ApplyEditorView();

  // Climbs from a clicked widget to the nearest editor-handled name and forwards
  // the matching event to editor_sink. Returns true if it consumed the click.
  bool RouteEditorClick(ugui::wid target);

  void ApplyMenuVisibility() {
    SetStyleField(
        "menu",
        [](ugui::Style& s, float v) {
          s.visibility = v > 0.5f ? ugui::Visibility::kVisible : ugui::Visibility::kCollapsed;
        },
        menu_open ? 1.0f : 0.0f);
    // Settings is a sub-view: the button column and the controls panel swap, and
    // the title follows so "Settings" reads as its own screen.
    SetVisible("menu_buttons", !settings_open);
    SetVisible("menu_settings", settings_open);
    ugui::SetText(ui.FindWidget("menu_title"), settings_open ? "Settings" : "Paused");
  }
};

namespace {
const ugui::Color kEdGold = ugui::Color::FromRgba8(0xff, 0xcc, 0x55, 0xff);
const ugui::Color kEdTabIdle = ugui::Color::FromRgba8(0xff, 0xff, 0xff, 0x0c);
const ugui::Color kEdTabActive = ugui::Color::FromRgba8(0xff, 0xcc, 0x55, 0x33);
const ugui::Color kEdRowIdle = ugui::Color::FromRgba8(0xff, 0xff, 0xff, 0x08);
const ugui::Color kEdRowArmed = ugui::Color::FromRgba8(0xff, 0xcc, 0x55, 0x3a);
const ugui::Color kEdTextIdle = ugui::Color::FromRgba8(0xaa, 0xb2, 0xc6, 0xff);
const ugui::Color kEdSearchHint = ugui::Color::FromRgba8(0x6b, 0x74, 0x88, 0xff);
const ugui::Color kEdSearchText = ugui::Color::FromRgba8(0xe8, 0xec, 0xf6, 0xff);
}  // namespace

void GameUi::Impl::ApplyEditorView() {
  // On the active<->inactive edge, hide the gameplay HUD while editing and
  // restore it on exit (the editor has its own reticle and chrome).
  if (editor.active != editor_prev_active) {
    const bool hud = !editor.active;
    SetVisible("topbar", hud);
    SetVisible("crosshair", hud);
    SetVisible("vitals", hud);
    SetVisible("readout", hud);
    editor_prev_active = editor.active;
  }
  SetVisible("editor_root", editor.active);
  if (!editor.active) return;

  // Toolbar hint reflects the current mode / armed brush.
  ugui::SetText(ui.FindWidget("ed_tb_hint"),
                editor.brush.empty() ? "select / build" : ("brush: " + editor.brush).c_str());

  // Category tabs: fill labels, collapse the unused pool slots, gold the active.
  for (int i = 0; i < kEditorCategoryTabs; ++i) {
    const std::string id = "btn_cat" + std::to_string(i);
    if (i < static_cast<int>(editor.categories.size())) {
      ugui::SetText(ui.FindWidget(id.c_str()), editor.categories[i].c_str());
      SetVisible(id.c_str(), true);
      const bool on = i == editor.category;
      SetBackground(id.c_str(), on ? kEdTabActive : kEdTabIdle);
      SetTextColor(id.c_str(), on ? kEdGold : kEdTextIdle);
    } else {
      SetVisible(id.c_str(), false);
    }
  }

  // Search box: typed text (with a caret while focused) or the placeholder.
  {
    std::string shown;
    bool placeholder = editor.search.empty() && !editor.search_focused;
    if (placeholder) {
      shown = "Search assets...";
    } else {
      shown = editor.search;
      if (editor.search_focused) shown += "|";
    }
    ugui::SetText(ui.FindWidget("ed_search_text"), shown.c_str());
    SetTextColor("ed_search_text", placeholder ? kEdSearchHint : kEdSearchText);
  }

  // Result count.
  {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d match%s", editor.result_count,
                  editor.result_count == 1 ? "" : "es");
    ugui::SetText(ui.FindWidget("ed_result_count"), buf);
  }

  // Asset rows: name + subtitle, collapse the unused pool, highlight the armed.
  for (int i = 0; i < kEditorBrowserRows; ++i) {
    const std::string row = "ed_row" + std::to_string(i);
    if (i < static_cast<int>(editor.rows.size())) {
      ugui::SetText(ui.FindWidget((row + "_name").c_str()), editor.rows[i].name.c_str());
      ugui::SetText(ui.FindWidget((row + "_sub").c_str()), editor.rows[i].subtitle.c_str());
      SetVisible(row.c_str(), true);
      SetBackground(row.c_str(), editor.rows[i].armed ? kEdRowArmed : kEdRowIdle);
    } else {
      SetVisible(row.c_str(), false);
    }
  }

  // Selection reticle: a fixed 64px corner bracket centred on the selected
  // object's screen position, so it reads clearly whatever the object's size.
  const bool bracket = editor.has_selection && editor.sel_on_screen;
  SetVisible("ed_select", bracket);
  if (bracket) {
    constexpr float kHalf = 32.0f;
    SetStyleField(
        "ed_select", [](ugui::Style& s, float v) { s.left_offset = ugui::Length::Px(v); },
        editor.sel_screen[0] - kHalf);
    SetStyleField(
        "ed_select", [](ugui::Style& s, float v) { s.top = ugui::Length::Px(v); },
        editor.sel_screen[1] - kHalf);
  }

  // Inspector with a live selection, the controls card otherwise, so the right
  // dock is always useful and the controls stay discoverable.
  SetVisible("editor_inspector", editor.has_selection);
  SetVisible("editor_help", !editor.has_selection);
  if (editor.has_selection) {
    ugui::SetText(ui.FindWidget("ed_insp_title"), editor.sel_title.c_str());
    ugui::SetText(ui.FindWidget("ed_insp_sub"), editor.sel_subtitle.c_str());
    char b[96];
    std::snprintf(b, sizeof(b), "pos   %.1f   %.1f   %.1f", editor.sel_pos[0], editor.sel_pos[1],
                  editor.sel_pos[2]);
    ugui::SetText(ui.FindWidget("ed_insp_pos"), b);
    std::snprintf(b, sizeof(b), "yaw   %.0f deg", editor.sel_yaw_deg);
    ugui::SetText(ui.FindWidget("ed_insp_rot"), b);
    std::snprintf(b, sizeof(b), "scale   %.2fx", editor.sel_scale);
    ugui::SetText(ui.FindWidget("ed_insp_scale"), b);
  }

  // Status bar.
  ugui::SetText(ui.FindWidget("ed_status_left"), editor.status.c_str());
  {
    char b[96];
    std::snprintf(b, sizeof(b), "%d placed     F4 exit     right-drag to fly", editor.object_count);
    ugui::SetText(ui.FindWidget("ed_status_right"), b);
  }
}

bool GameUi::Impl::RouteEditorClick(ugui::wid target) {
  if (!editor_sink || !editor.active) return false;
  // Climb from the clicked widget to the nearest editor-handled name; clicking
  // a row's text resolves to its parent row, a button's glyph to the button.
  ugui::wid w = target;
  for (int depth = 0; depth < 6 && w.valid(); ++depth) {
    const ugui::WidgetNode* n = ui.world().Get<ugui::WidgetNode>(w);
    if (n) {
      const std::string name = n->name.c_str();
      auto suffix_index = [&](const char* prefix) -> int {
        const size_t pl = std::strlen(prefix);
        if (name.size() > pl && name.compare(0, pl, prefix) == 0)
          return std::atoi(name.c_str() + pl);
        return -1;
      };
      EditorUiEvent e;
      if (int i = suffix_index("btn_tool"); i >= 0) {
        e.kind = EditorUiEvent::Kind::kTool;
        e.index = i;
        editor_sink(e);
        return true;
      }
      if (int i = suffix_index("btn_cat"); i >= 0) {
        e.kind = EditorUiEvent::Kind::kCategory;
        e.index = i;
        editor_sink(e);
        return true;
      }
      if (name.rfind("ed_row", 0) == 0) {
        // ed_row3 or ed_row3_name/_sub all map to row 3.
        e.kind = EditorUiEvent::Kind::kPickRow;
        e.index = std::atoi(name.c_str() + 6);
        editor_sink(e);
        return true;
      }
      if (name == "ed_search" || name == "ed_search_text") {
        e.kind = EditorUiEvent::Kind::kTool;
        e.index = 8;  // kToolFocusSearch
        editor_sink(e);
        return true;
      }
      if (name == "ed_search_clear") {
        e.kind = EditorUiEvent::Kind::kTool;
        e.index = 9;  // kToolClearSearch
        editor_sink(e);
        return true;
      }
      if (name == "btn_scroll_up") {
        e.kind = EditorUiEvent::Kind::kScroll;
        e.index = -1;
        editor_sink(e);
        return true;
      }
      if (name == "btn_scroll_down") {
        e.kind = EditorUiEvent::Kind::kScroll;
        e.index = 1;
        editor_sink(e);
        return true;
      }
    }
    const ugui::Hierarchy* h = ui.world().Get<ugui::Hierarchy>(w);
    w = h ? h->parent : ugui::wid{};
  }
  return false;
}

GameUi::GameUi() : impl_(std::make_unique<Impl>()) {}
GameUi::~GameUi() { Shutdown(); }

bool GameUi::Initialize(Window& window, render::Renderer& renderer) {
  render::Device* device = renderer.device();
  if (!device || device->is_stub()) return false;

  impl_->host.window_width = static_cast<float>(window.width());
  impl_->host.window_height = static_cast<float>(window.height());

  ugui::UIConfig cfg;
  cfg.draw_data = true;
  cfg.external_window = &impl_->host;
  cfg.width = static_cast<int>(window.width());
  cfg.height = static_cast<int>(window.height());
  if (!impl_->ui.Init(cfg)) {
    REC_WARN("ultragui init failed");
    return false;
  }

  if (const char* font_path = FindFont()) {
    impl_->font = impl_->ui.LoadFont(font_path);
    impl_->ui.set_default_font(impl_->font);
    REC_INFO("ultragui font: {}", font_path);
  } else {
    REC_WARN("no ui font found (set RECREATION_UI_FONT), hud text will be blank");
  }

  ui::GuiRenderBackend::InitInfo bi;
  bi.instance = device->instance();
  bi.physical_device = device->physical_device();
  bi.device = device->device();
  bi.queue_family = device->graphics_family();
  bi.queue = device->graphics_queue();
  bi.color_format = renderer.swapchain_format();
  bi.frames_in_flight = 2;
  if (!impl_->backend.Init(bi)) {
    REC_WARN("ultragui vulkan backend init failed");
    impl_->ui.Shutdown();
    return false;
  }
  impl_->ui.set_texture_backend(&impl_->backend);

  std::string doc = BuildUi();
  impl_->ui.LoadUiString(doc.c_str(), "hud");

  Impl* impl = impl_.get();
  impl_->ui.input().set_on_click([impl](ugui::wid w, ugui::MouseButton btn) {
    if (btn != ugui::MouseButton::kLeft) return;
    if (impl->RouteEditorClick(w)) return;  // editor overlay owns this click
    ugui::WidgetNode* n = impl->ui.world().Get<ugui::WidgetNode>(w);
    if (!n) return;
    if (n->name == "btn_resume") {
      impl->menu_open = false;
      impl->settings_open = false;
      impl->ApplyMenuVisibility();
    } else if (n->name == "btn_settings") {
      impl->settings_open = true;
      impl->ApplyMenuVisibility();
    } else if (n->name == "btn_settings_back") {
      impl->settings_open = false;
      impl->ApplyMenuVisibility();
    } else if (n->name == "btn_quit") {
      impl->quit_requested = true;
    }
  });

  // Editor overlay starts collapsed; the engine reveals it on F4. The category
  // strip wraps to a tag cloud, which the markup grammar cannot express.
  impl_->SetVisible("editor_root", false);
  if (ugui::wid cats = impl_->ui.FindWidget("ed_cats"); cats.valid()) {
    if (ugui::StyleC* sc = impl_->ui.world().Get<ugui::StyleC>(cats)) {
      ugui::Style style = sc->style;
      style.flex_wrap = ugui::FlexWrap::kWrap;
      ugui::SetStyle(impl_->ui.world(), cats, style);
    }
  }

  // Debug aid: RECREATION_UI_MENU opens the pause menu at startup.
  if (std::getenv("RECREATION_UI_MENU")) impl_->menu_open = true;
  impl_->ApplyMenuVisibility();  // menu starts hidden unless forced open
  impl_->initialized = true;
  REC_INFO("ultragui hud initialized (draw-data mode)");
  return true;
}

void GameUi::Shutdown() {
  if (!impl_ || !impl_->initialized) return;
  impl_->backend.Shutdown();
  impl_->ui.Shutdown();
  impl_->initialized = false;
}

void GameUi::ToggleMenu() {
  if (!impl_->initialized) return;
  impl_->menu_open = !impl_->menu_open;
  impl_->settings_open = false;  // always reopen on the main pause screen
  impl_->ApplyMenuVisibility();
}

bool GameUi::menu_open() const { return impl_->initialized && impl_->menu_open; }
bool GameUi::quit_requested() const { return impl_->initialized && impl_->quit_requested; }

void GameUi::SetQuest(const HudQuest& quest) {
  if (impl_->initialized) impl_->quest = quest;
}

void GameUi::FlashQuestUpdate(const std::string& message) {
  if (!impl_->initialized) return;
  impl_->toast_text = message;
  impl_->toast_age = 0.0f;
}

void GameUi::SetActivatePrompt(const std::string& prompt) {
  if (impl_->initialized) impl_->activate_prompt = prompt;
}

void GameUi::SetHudVisible(bool visible) {
  if (!impl_->initialized) return;
  impl_->SetVisible("topbar", visible);  // compass
  impl_->SetVisible("crosshair", visible);
  impl_->SetVisible("vitals", visible);   // health / magicka / stamina bars
  impl_->SetVisible("readout", visible);  // fps / coords / heading
}

void GameUi::SetObjectiveMarker(bool active, float bearing_deg, float distance_m) {
  if (!impl_->initialized) return;
  impl_->marker_active = active;
  impl_->marker_bearing = bearing_deg;
  impl_->marker_distance = distance_m;
}

void GameUi::SetDialogue(const DialogueView& dialogue) {
  if (impl_->initialized) impl_->dialogue = dialogue;
}

void GameUi::SetContainer(const ContainerView& container) {
  if (impl_->initialized) impl_->container = container;
}

void GameUi::SetJournal(bool open, const std::vector<HudQuest>& quests, int selected) {
  if (!impl_->initialized) return;
  impl_->journal_open = open;
  impl_->journal = quests;
  impl_->journal_selected = selected;
}

void GameUi::SetEditorView(const EditorView& view) {
  if (impl_->initialized) impl_->editor = view;
}

void GameUi::SetEditorEventSink(std::function<void(const EditorUiEvent&)> sink) {
  if (impl_->initialized) impl_->editor_sink = std::move(sink);
}

void GameUi::Build(Window& window, render::Renderer&, FlyCamera& camera, f32 frame_delta,
                   render::FrameView* view) {
  if (!impl_->initialized) return;
  Impl* impl = impl_.get();

  impl->host.window_width = static_cast<float>(window.width());
  impl->host.window_height = static_cast<float>(window.height());

  // Feed the per-frame input snapshot into ultragui's queue.
  const InputState& in = window.input();
  ugui::InputQueue& q = impl->ui.platform()->input_queue();
  q.PushMove({in.mouse_x, in.mouse_y});
  const ugui::MouseButton buttons[3] = {ugui::MouseButton::kLeft, ugui::MouseButton::kRight,
                                        ugui::MouseButton::kMiddle};
  const MouseButton rec_buttons[3] = {MouseButton::kLeft, MouseButton::kRight,
                                      MouseButton::kMiddle};
  for (int i = 0; i < 3; ++i) {
    bool down = in.button(rec_buttons[i]);
    if (down != impl->prev_mouse[i]) q.PushButton(buttons[i], down);
    impl->prev_mouse[i] = down;
  }
  if (in.wheel != 0.0f) q.PushScroll({0.0f, in.wheel});

  // --- Drive HUD values from real engine state ---
  // Compass heading from the camera's facing direction.
  Vec3 fwd = camera.forward();
  float heading = std::atan2(fwd.x, -fwd.z) * 57.29578f;
  if (heading < 0.0f) heading += 360.0f;
  impl->SetStyleField(
      "compass_strip", [](ugui::Style& s, float v) { s.left_offset = ugui::Length::Px(v); },
      CompassStripLeft(heading));

  // Objective compass waypoint: a gold pip at the bearing to the active quest
  // marker, plus a distance readout under the compass. 70 px on the strip spans
  // 45 degrees, so the pip tracks the same scale as the cardinal labels.
  impl->SetVisible("quest_pip", impl->marker_active);
  impl->SetVisible("quest_marker_box", impl->marker_active);
  if (impl->marker_active) {
    float off = impl->marker_bearing / 45.0f * kCompassLabel;
    off = std::clamp(off, -(kCompassCenter - 8.0f), kCompassCenter - 8.0f);
    impl->SetStyleField(
        "quest_pip", [](ugui::Style& s, float v) { s.left_offset = ugui::Length::Px(v); },
        kCompassCenter + off - 4.5f);
    char mbuf[48];
    std::snprintf(mbuf, sizeof(mbuf), "%.0f m", impl->marker_distance);
    ugui::SetText(impl->ui.FindWidget("quest_marker_text"), mbuf);
  }

  // Stamina drains while sprinting (shift + movement), regenerates otherwise.
  bool moving = in.key(Key::kW) || in.key(Key::kA) || in.key(Key::kS) || in.key(Key::kD);
  bool sprinting = in.key(Key::kLeftShift) && moving;
  impl->stamina += (sprinting ? -0.45f : 0.30f) * frame_delta;
  impl->stamina = std::clamp(impl->stamina, 0.0f, 1.0f);
  impl->SetStyleField(
      "bar_stamina_fill", [](ugui::Style& s, float v) { s.width = ugui::Length::Pct(v); },
      impl->stamina * 100.0f);

  // Readout text.
  char buf[160];
  std::snprintf(buf, sizeof(buf), "%.0f fps", frame_delta > 0 ? 1.0f / frame_delta : 0.0f);
  ugui::SetText(impl->ui.FindWidget("hud_fps"), buf);
  Vec3 pos = camera.position();
  std::snprintf(buf, sizeof(buf), "x %.0f   y %.0f   z %.0f", pos.x, pos.y, pos.z);
  ugui::SetText(impl->ui.FindWidget("hud_coords"), buf);
  const char* card = kCardinals[static_cast<int>(std::fmod(heading + 22.5f, 360.0f) / 45.0f) % 8];
  std::snprintf(buf, sizeof(buf), "%s  %.0f deg", card, heading);
  ugui::SetText(impl->ui.FindWidget("hud_heading"), buf);

  // --- Quest HUD ---
  const bool has_quest = !impl->quest.title.empty();
  impl->SetVisible("questtracker", has_quest);
  if (has_quest) ugui::SetText(impl->ui.FindWidget("quest_title"), impl->quest.title.c_str());
  for (int i = 0; i < kQuestObjectiveRows; ++i) {
    std::string row = "quest_obj" + std::to_string(i);
    if (has_quest && static_cast<size_t>(i) < impl->quest.objectives.size()) {
      const HudQuest::Objective& o = impl->quest.objectives[i];
      // A check for done objectives, a bullet for the rest.
      std::string line = (o.completed ? "✓  " : "•  ") + o.text;
      ugui::SetText(impl->ui.FindWidget(row.c_str()), line.c_str());
      impl->SetVisible(row.c_str(), true);
    } else {
      impl->SetVisible(row.c_str(), false);
    }
  }

  // The "quest updated" banner fades out after a few seconds.
  impl->toast_age += frame_delta;
  const bool toast_on = impl->toast_age < kToastSeconds && !impl->toast_text.empty();
  impl->SetVisible("quest_toast_box", toast_on);
  if (toast_on) ugui::SetText(impl->ui.FindWidget("quest_toast"), impl->toast_text.c_str());

  // Centered activation prompt ("Talk to Ralof", "Open the gate", ...).
  const bool prompt_on = !impl->activate_prompt.empty();
  impl->SetVisible("activate_box", prompt_on);
  if (prompt_on)
    ugui::SetText(impl->ui.FindWidget("activate_prompt"), impl->activate_prompt.c_str());

  // Dialogue panel: speaker + last NPC line + numbered player topics.
  const DialogueView& dlg = impl->dialogue;
  impl->SetVisible("dialogue_box", dlg.open);
  if (dlg.open) {
    ugui::SetText(impl->ui.FindWidget("dialogue_speaker"), dlg.speaker.c_str());
    ugui::SetText(impl->ui.FindWidget("dialogue_npc"), dlg.npc_line.c_str());
    for (int i = 0; i < kDialogueOptionRows; ++i) {
      const std::string row = "dialogue_opt" + std::to_string(i);
      if (i < static_cast<int>(dlg.options.size())) {
        const std::string line = std::to_string(i + 1) + ". " + dlg.options[i];
        ugui::SetText(impl->ui.FindWidget(row.c_str()), line.c_str());
        impl->SetVisible(row.c_str(), true);
      } else {
        impl->SetVisible(row.c_str(), false);
      }
    }
  }

  // Container loot panel: the container's name and a fixed pool of item rows.
  const ContainerView& cont = impl->container;
  impl->SetVisible("container_box", cont.open);
  if (cont.open) {
    ugui::SetText(impl->ui.FindWidget("container_head"), cont.name.c_str());
    for (int i = 0; i < kContainerRows; ++i) {
      const std::string row = "container_item" + std::to_string(i);
      if (i < static_cast<int>(cont.items.size())) {
        std::string line = cont.items[i].name;
        if (cont.items[i].count > 1) line += "  x" + std::to_string(cont.items[i].count);
        ugui::SetText(impl->ui.FindWidget(row.c_str()), line.c_str());
        impl->SetVisible(row.c_str(), true);
      } else {
        impl->SetVisible(row.c_str(), false);
      }
    }
    // An empty chest still gets a line so it does not read as a bug.
    if (cont.items.empty()) {
      ugui::SetText(impl->ui.FindWidget("container_item0"), "(empty)");
      impl->SetVisible("container_item0", true);
    }
  }

  // Quest journal: a numbered list of active quests; an arrow marks the tracked
  // one and its objectives are listed below.
  impl->SetVisible("journal_box", impl->journal_open);
  if (impl->journal_open) {
    for (int i = 0; i < kJournalRows; ++i) {
      const std::string row = "journal_q" + std::to_string(i);
      if (i < static_cast<int>(impl->journal.size())) {
        const std::string mark = i == impl->journal_selected ? "▶ " : "   ";
        const std::string line = mark + std::to_string(i + 1) + ". " + impl->journal[i].title;
        ugui::SetText(impl->ui.FindWidget(row.c_str()), line.c_str());
        impl->SetVisible(row.c_str(), true);
      } else {
        impl->SetVisible(row.c_str(), false);
      }
    }
    const HudQuest* sel = (impl->journal_selected >= 0 &&
                           impl->journal_selected < static_cast<int>(impl->journal.size()))
                              ? &impl->journal[impl->journal_selected]
                              : nullptr;
    for (int i = 0; i < kJournalObjRows; ++i) {
      const std::string row = "journal_obj" + std::to_string(i);
      if (sel && i < static_cast<int>(sel->objectives.size())) {
        const std::string line =
            (sel->objectives[i].completed ? "✓  " : "•  ") + sel->objectives[i].text;
        ugui::SetText(impl->ui.FindWidget(row.c_str()), line.c_str());
        impl->SetVisible(row.c_str(), true);
      } else {
        impl->SetVisible(row.c_str(), false);
      }
    }
  }

  // Map editor overlay (asset browser, toolbar, inspector, status, reticle).
  impl->ApplyEditorView();

  // Produce the draw list (input routing + layout + paint, no GPU work).
  const ugui::DrawData& dd = impl->ui.RenderDrawData();
  impl->draw_data = &dd;

  // Upload the glyph atlas if it grew this frame.
  if (impl->ui.text_engine().atlas_revision() != impl->font_revision) {
    ugui::Vec2 as = impl->ui.text_engine().atlas_size();
    impl->backend.UpdateFontAtlas(impl->ui.text_engine().atlas_pixels(), static_cast<u32>(as.x),
                                  static_cast<u32>(as.y));
    impl->font_revision = impl->ui.text_engine().atlas_revision();
  }

  impl->backend.NewFrame();
  view->hud_draw = [impl](VkCommandBuffer cmd) {
    if (impl->draw_data) impl->backend.Render(*impl->draw_data, cmd);
  };
}

}  // namespace rec

#else  // !RECREATION_HAS_UGUI

namespace rec {

struct GameUi::Impl {};
GameUi::GameUi() = default;
GameUi::~GameUi() = default;
bool GameUi::Initialize(Window&, render::Renderer&) { return false; }
void GameUi::Shutdown() {}
void GameUi::Build(Window&, render::Renderer&, FlyCamera&, f32, render::FrameView*) {}
void GameUi::SetQuest(const HudQuest&) {}
void GameUi::FlashQuestUpdate(const std::string&) {}
void GameUi::SetActivatePrompt(const std::string&) {}
void GameUi::SetHudVisible(bool) {}
void GameUi::SetObjectiveMarker(bool, float, float) {}
void GameUi::SetDialogue(const DialogueView&) {}
void GameUi::SetContainer(const ContainerView&) {}
void GameUi::SetJournal(bool, const std::vector<HudQuest>&, int) {}
void GameUi::SetEditorView(const EditorView&) {}
void GameUi::SetEditorEventSink(std::function<void(const EditorUiEvent&)>) {}
void GameUi::ToggleMenu() {}
bool GameUi::menu_open() const { return false; }
bool GameUi::quit_requested() const { return false; }

}  // namespace rec

#endif  // RECREATION_HAS_UGUI
