#include "game_ui.h"

#include "fly_camera.h"

#if defined(RECREATION_HAS_UGUI)

#include <ugui/core/color.h>
#include <ugui/style/style.h>
#include <ugui/ultragui.h>
#include <ugui/widgets/image.h>
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

// NEXUS main menu.
constexpr int kMenuUniverses = 3;  // Skyrim, Fallout 4, Starfield
constexpr int kMenuNavItems = 6;   // PLAY MULTIPLAYER MODS SETTINGS PROFILE QUIT
constexpr int kMenuModRows = 16;   // pooled rows on the Mods sub-screen

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

// Editor icon glyphs, composed from panels: the UI font (Noto Sans) has no
// symbol set, so toolbar / tree / inspector icons are drawn as little stacks of
// filled and outlined rectangles inside an 18x18 relative box, the same trick
// the crosshair uses. `col` is the mark colour.
std::string Glyph(const std::string& k, const char* col) {
  std::string c;
  char b[256];
  auto R = [&](float x, float y, float w, float h, const char* cc, float r = 0) {
    std::snprintf(b, sizeof(b),
                  "panel { position: absolute; left: %g; top: %g; width: %g; height: %g;"
                  " background: %s; corner-radius: %g; }\n",
                  x, y, w, h, cc, r);
    c += b;
  };
  auto O = [&](float x, float y, float w, float h, const char* cc, float bw, float r) {
    std::snprintf(b, sizeof(b),
                  "panel { position: absolute; left: %g; top: %g; width: %g; height: %g;"
                  " border-color: %s; border-width: %g; corner-radius: %g; }\n",
                  x, y, w, h, cc, bw, r);
    c += b;
  };
  if (k == "select") { R(4, 3, 2, 11, col); R(4, 3, 8, 2, col); R(8, 8, 5, 2, col); }
  else if (k == "move") { R(8, 2, 2, 14, col, 1); R(2, 8, 14, 2, col, 1); }
  else if (k == "rotate") { O(3, 3, 12, 12, col, 2, 6); R(13, 1, 4, 2, col); }
  else if (k == "scale") { O(3, 7, 8, 8, col, 1.5f, 2); R(11, 3, 4, 4, col, 1); }
  else if (k == "hand") { R(6, 7, 6, 8, col, 2); R(6, 4, 2, 5, col, 1); R(9, 3, 2, 6, col, 1); R(12, 5, 2, 4, col, 1); }
  else if (k == "terrain") { R(2, 11, 4, 5, col, 1); R(7, 7, 4, 9, col, 1); R(12, 9, 4, 7, col, 1); }
  else if (k == "paint") { R(4, 3, 9, 5, col, 2); R(8, 8, 2, 7, col, 1); }
  else if (k == "play") { R(5, 4, 3, 10, col); R(8, 6, 3, 6, col); R(11, 8, 2, 2, col); }
  else if (k == "save") { O(3, 3, 12, 12, col, 1.5f, 2); R(6, 3, 6, 3, col); R(6, 10, 6, 2, col); }
  else if (k == "undo") { R(5, 8, 9, 2, col); R(5, 5, 2, 3, col); R(5, 10, 2, 3, col); R(12, 5, 2, 4, col); }
  else if (k == "redo") { R(4, 8, 9, 2, col); R(11, 5, 2, 3, col); R(11, 10, 2, 3, col); R(4, 5, 2, 4, col); }
  else if (k == "gear") { O(4, 4, 10, 10, col, 2, 5); R(8, 1, 2, 3, col); R(8, 14, 2, 3, col); R(1, 8, 3, 2, col); R(14, 8, 3, 2, col); }
  else if (k == "cube") { O(4, 5, 10, 9, col, 1.5f, 1); R(4, 5, 10, 3, col); }
  else if (k == "folder") { R(3, 6, 12, 8, col, 1); R(3, 4, 6, 3, col, 1); }
  else if (k == "sphere") { O(3, 3, 12, 12, col, 1.5f, 6); R(6, 5, 3, 3, "#ffffff66", 2); }
  else if (k == "magnify") { O(3, 3, 9, 9, col, 1.5f, 5); R(11, 11, 4, 2, col, 1); }
  else if (k == "caret") { R(5, 7, 8, 2, col); R(6, 9, 6, 2, col); R(8, 11, 2, 2, col); }
  else if (k == "triup") { R(8, 7, 2, 2, col); R(6, 9, 6, 2, col); R(5, 11, 8, 2, col); }
  else if (k == "tridown") { R(5, 7, 8, 2, col); R(6, 9, 6, 2, col); R(8, 11, 2, 2, col); }
  else if (k == "lock") { R(4, 8, 10, 7, col, 1); O(6, 4, 6, 6, col, 1.5f, 3); }
  else if (k == "grid") { R(4, 4, 4, 4, col, 1); R(10, 4, 4, 4, col, 1); R(4, 10, 4, 4, col, 1); R(10, 10, 4, 4, col, 1); }
  else if (k == "plus") { R(8, 4, 2, 10, col); R(4, 8, 10, 2, col); }
  else if (k == "kebab") { R(8, 3, 2, 2, col, 1); R(8, 8, 2, 2, col, 1); R(8, 13, 2, 2, col, 1); }
  else if (k == "eye") { O(3, 6, 12, 7, col, 1.5f, 4); R(7, 8, 4, 4, col, 2); }
  else if (k == "minwin") { R(4, 9, 10, 2, col); }
  else if (k == "maxwin") { O(4, 4, 10, 10, col, 1.5f, 1); }
  return "panel { position: relative; width: 18; height: 18; " + c + " }\n";
}

// The map editor overlay, styled as a Creation-Kit-style dock layout: a top
// toolbar (logo + labelled tool cluster + window controls), a left scene/assets
// dock with a hierarchy tree, a right inspector, a bottom asset browser between
// the side docks, and a status bar. Everything is pooled (fixed widget counts
// filled and toggled each frame) and starts hidden; the engine collapses
// editor_root until the editor is on. Names are matched by the click router
// (btn_tool*, btn_giz*, btn_ltab*, ed_trow*, btn_btab*, cl_row*, card*, ...).
std::string BuildEditorSection() {
  const char* AC = "#6c7bf5";   // accent indigo
  const char* TXP = "#e6e9f2";  // primary text / icons
  const char* TXS = "#9aa3b5";  // secondary text
  const char* TXM = "#6b7488";  // muted text
  std::string s;
  char buf[2048];

  // --- reticle, marquee box and selection bracket (static, world overlays) ---
  s += R"(
  panel editor_root {
    position: absolute; top: 0; left: 0; width: 100vw; height: 100vh;

    panel ed_reticle {
      position: absolute; top: 0; left: 0; width: 100vw; height: 100vh;
      layout: column; justify: center; align: center;
      panel { width: 18; height: 18; position: relative;
        panel { position: absolute; left: 8; top: 2; width: 2; height: 6; background: #ffffffcc; }
        panel { position: absolute; left: 8; top: 10; width: 2; height: 6; background: #ffffffcc; }
        panel { position: absolute; left: 2; top: 8; width: 6; height: 2; background: #ffffffcc; }
        panel { position: absolute; left: 10; top: 8; width: 6; height: 2; background: #ffffffcc; }
      }
    }

    panel ed_marquee {
      position: absolute; left: 0; top: 0; width: 0; height: 0;
      background: #6c7bf524; border-color: #6c7bf5cc; border-width: 1;
    }

    panel ed_select {
      position: absolute; left: 0; top: 0; width: 64; height: 64;
      panel { position: absolute; left: 0; top: 0; width: 16; height: 2; background: #6c7bf5; }
      panel { position: absolute; left: 0; top: 0; width: 2; height: 16; background: #6c7bf5; }
      panel { position: absolute; left: 48; top: 0; width: 16; height: 2; background: #6c7bf5; }
      panel { position: absolute; left: 62; top: 0; width: 2; height: 16; background: #6c7bf5; }
      panel { position: absolute; left: 0; top: 62; width: 16; height: 2; background: #6c7bf5; }
      panel { position: absolute; left: 0; top: 48; width: 2; height: 16; background: #6c7bf5; }
      panel { position: absolute; left: 48; top: 62; width: 16; height: 2; background: #6c7bf5; }
      panel { position: absolute; left: 62; top: 48; width: 2; height: 16; background: #6c7bf5; }
    }
)";

  // --- viewport overlays: gizmo bar, perspective chip, axis gizmo ---
  std::snprintf(buf, sizeof(buf),
                "\n    panel ed_gizmobar { position: absolute; left: %g; top: %g; layout: row;"
                " align: center; gap: 2; background: #14171fe8; corner-radius: 10; padding: 4;"
                " border-color: #ffffff14; border-width: 1;\n",
                kEdSceneW + 16.0f, kEdToolbarH + 14.0f);
  s += buf;
  const char* giz[4] = {"hand", "move", "rotate", "scale"};
  for (int i = 0; i < 4; ++i) {
    s += "      panel btn_giz" + std::to_string(i) +
         " { padding: 6; corner-radius: 7; background: #ffffff00; cursor: pointer;"
         " :hover { background: #ffffff14; }\n        " +
         Glyph(giz[i], TXP) + "      }\n";
  }
  s += "    }\n";

  std::snprintf(buf, sizeof(buf),
                "\n    panel ed_persp { position: absolute; right: %g; top: %g; layout: row;"
                " align: center; gap: 8; background: #14171fe8; corner-radius: 8; padding: 7 11;"
                " border-color: #ffffff14; border-width: 1;\n"
                "      text { text: \"Perspective\"; font-size: 12; color: %s; }\n      %s    }\n",
                kEdInspectorW + 16.0f, kEdToolbarH + 14.0f, TXS, Glyph("caret", TXS).c_str());
  s += buf;

  std::snprintf(buf, sizeof(buf),
                "\n    panel ed_axis { position: absolute; right: %g; top: %g; width: 60; height: 60;\n"
                "      panel { position: absolute; left: 28; top: 8; width: 2; height: 22; background: #57bd6a; }\n"
                "      text { position: absolute; left: 25; top: 0; font-size: 11; color: #57bd6a; text: \"Y\"; }\n"
                "      panel { position: absolute; left: 29; top: 28; width: 22; height: 2; background: #e5564b; }\n"
                "      text { position: absolute; left: 50; top: 22; font-size: 11; color: #e5564b; text: \"X\"; }\n"
                "      panel { position: absolute; left: 8; top: 28; width: 22; height: 2; background: #4d8df0; }\n"
                "      text { position: absolute; left: 1; top: 33; font-size: 11; color: #4d8df0; text: \"Z\"; }\n"
                "      panel { position: absolute; left: 26; top: 26; width: 6; height: 6; corner-radius: 3; background: #cfd6e6; }\n    }\n",
                kEdInspectorW + 26.0f, kEdToolbarH + 50.0f);
  s += buf;

  // --- top toolbar ---
  std::snprintf(buf, sizeof(buf),
                "\n    panel editor_toolbar { position: absolute; top: 0; left: 0; width: 100vw;"
                " height: %g; layout: row; align: center; justify: space-between; padding: 0 14;"
                " background: #1b1e27f8; border-color: #ffffff12; border-width: 1;\n",
                kEdToolbarH);
  s += buf;
  s += R"(      panel { layout: row; align: center; gap: 11;
        panel { width: 30; height: 30; corner-radius: 8; background: #6c7bf5; layout: column;
          justify: center; align: center; text { text: "R"; font-size: 17; color: #ffffff; } }
        text { text: "RECREATION"; font-size: 14; color: #e6e9f2; letter-spacing: 2; }
      }
      panel ed_tb_tools { layout: row; align: center; gap: 3;
)";
  const char* tlabel[kEdToolBtns] = {"Select", "Move", "Rotate", "Terrain", "Paint",
                                     "Play",   "Save", "Undo",   "Redo"};
  const char* tkind[kEdToolBtns] = {"select", "move", "rotate", "terrain", "paint",
                                    "play",   "save", "undo",   "redo"};
  for (int i = 0; i < kEdToolBtns; ++i) {
    if (i == 5)
      s += "        panel { width: 1; height: 28; background: #ffffff1c; margin: 0 6; }\n";
    s += "        panel btn_tool" + std::to_string(i) +
         " { layout: column; align: center; justify: center; gap: 3; padding: 5 8;"
         " corner-radius: 9; cursor: pointer; background: #ffffff00; :hover { background: #ffffff12; }\n          " +
         Glyph(tkind[i], TXP) + "          text btn_tool" + std::to_string(i) + "_lbl { text: \"" +
         tlabel[i] + "\"; font-size: 10; color: " + TXS + "; }\n        }\n";
  }
  s += "      }\n";  // close ed_tb_tools
  // Right cluster: world chip, settings, help, window controls.
  s += "      panel { layout: row; align: center; gap: 9;\n";
  s += "        panel { layout: row; align: center; gap: 7; background: #0e1016; corner-radius: 8;"
       " padding: 6 10; border-color: #ffffff14; border-width: 1;\n"
       "          text { text: \"Skyrim\"; font-size: 12; color: #cfd6e6; }\n          " +
       Glyph("caret", TXS) + "        }\n";
  s += "        panel { padding: 6; corner-radius: 7; background: #ffffff00; cursor: pointer;"
       " :hover { background: #ffffff12; }\n          " +
       Glyph("gear", TXS) + "        }\n";
  s += "        button { text: \"?\"; font-size: 15; color: #9aa3b5; padding: 4 9; corner-radius: 7;"
       " background: #ffffff00; cursor: pointer; :hover { background: #ffffff12; } }\n";
  s += "        panel { layout: row; align: center; gap: 2; margin: 0 0 0 4;\n          " +
       Glyph("minwin", TXM) + "          " + Glyph("maxwin", TXM) +
       "          text { text: \"x\"; font-size: 14; color: #9aa3b5; padding: 0 4; }\n        }\n";
  s += "      }\n    }\n";  // close right cluster, toolbar

  // --- left dock: scene tree / assets ---
  std::snprintf(buf, sizeof(buf),
                "\n    panel editor_scene { position: absolute; left: 0; top: %g; width: %g;"
                " bottom: %g; layout: column; align: start; background: #171a22f8;"
                " border-color: #ffffff12; border-width: 1;\n",
                kEdToolbarH, kEdSceneW, kEdStatusH);
  s += buf;
  s += R"(      panel { layout: row; align: center; padding: 4 10 0 10; width: 100%;
        panel btn_ltab0 { layout: column; align: center; gap: 6; padding: 9 12; cursor: pointer; background: #ffffff00;
          text btn_ltab0_t { text: "Scene"; font-size: 13; color: #e6e9f2; }
          panel btn_ltab0_ul { width: 38; height: 2; corner-radius: 1; background: #6c7bf5; }
        }
        panel btn_ltab1 { layout: column; align: center; gap: 6; padding: 9 12; cursor: pointer; background: #ffffff00;
          text btn_ltab1_t { text: "Assets"; font-size: 13; color: #6b7488; }
          panel btn_ltab1_ul { width: 38; height: 2; corner-radius: 1; background: #6c7bf500; }
        }
      }
      panel { width: 100%; height: 1; background: #ffffff10; }
      panel { layout: row; align: center; gap: 8; padding: 10 12 6 12; width: 100%;
        panel ed_scene_search { layout: row; align: center; gap: 7; flex-grow: 1; background: #0e1016;
          corner-radius: 8; padding: 7 10; border-color: #ffffff14; border-width: 1; cursor: text;
)";
  s += "          " + Glyph("magnify", TXM) +
       "          text ed_scene_search_text { text: \"Search scene...\"; font-size: 12; color: #6b7488; flex-grow: 1; }\n"
       "          button ed_scene_clear { text: \"x\"; font-size: 12; color: #6b7488; padding: 0 2;"
       " background: #ffffff00; cursor: pointer; :hover { color: #e6e9f2; } }\n        }\n";
  s += "        panel ed_scene_filter { padding: 7; corner-radius: 8; background: #0e1016;"
       " border-color: #ffffff14; border-width: 1; cursor: pointer; :hover { background: #ffffff12; }\n          " +
       Glyph("caret", TXM) + "        }\n      }\n";
  // Tree rows (pooled).
  s += "      panel ed_tree { layout: column; align: start; gap: 1; padding: 2 6; width: 100%;"
       " flex-grow: 1; overflow: hidden;\n";
  for (int i = 0; i < kEdTreeRows; ++i) {
    const std::string id = std::to_string(i);
    s += "        panel ed_trow" + id +
         " { layout: row; align: center; gap: 5; padding: 4 6; width: 100%; corner-radius: 6;"
         " cursor: pointer; background: #ffffff00; :hover { background: #ffffff10; }\n"
         "          panel ed_trow" + id + "_pad { width: 2; height: 1; }\n"
         "          button ed_trow" + id + "_exp { text: \"\"; font-size: 13; color: #9aa3b5;"
         " width: 14; text-align: center; background: #ffffff00; cursor: pointer; }\n"
         "          panel ed_trow" + id + "_ico { width: 11; height: 11; corner-radius: 3; background: #6b7488; }\n"
         "          text ed_trow" + id + "_name { text: \"\"; font-size: 12; color: #d6dbe7; flex-grow: 1; }\n"
         "          panel ed_trow" + id + "_eye { width: 12; height: 12; corner-radius: 6; background: #c8cfdd; cursor: pointer; }\n"
         "        }\n";
  }
  s += "      }\n";  // close ed_tree
  // Footer: add buttons + tree pager.
  s += "      panel { layout: row; align: center; justify: space-between; padding: 6 10; width: 100%;\n"
       "        panel { layout: row; align: center; gap: 6;\n";
  s += "          panel { padding: 6; corner-radius: 7; background: #ffffff0c; cursor: pointer; :hover { background: #ffffff18; }\n            " + Glyph("plus", TXS) + "          }\n";
  s += "          panel { padding: 6; corner-radius: 7; background: #ffffff0c; cursor: pointer; :hover { background: #ffffff18; }\n            " + Glyph("folder", TXS) + "          }\n";
  s += "          panel { padding: 6; corner-radius: 7; background: #ffffff0c; cursor: pointer; :hover { background: #ffffff18; }\n            " + Glyph("grid", TXS) + "          }\n";
  s += "        }\n        panel { layout: row; align: center; gap: 4;\n";
  s += "          panel btn_treeup { padding: 6; corner-radius: 7; background: #ffffff0c; cursor: pointer; :hover { background: #ffffff18; }\n            " + Glyph("triup", TXS) + "          }\n";
  s += "          panel btn_treedn { padding: 6; corner-radius: 7; background: #ffffff0c; cursor: pointer; :hover { background: #ffffff18; }\n            " + Glyph("tridown", TXS) + "          }\n";
  s += "        }\n      }\n    }\n";  // close footer, editor_scene

  // --- right dock: inspector ---
  auto section = [&](const char* title) {
    s += "        panel { layout: row; align: center; gap: 6; width: 100%; margin: 4 0 0 0;\n          " +
         Glyph("caret", TXS) + "          text { text: \"" + title +
         "\"; font-size: 12; color: " + TXP + "; letter-spacing: 1; flex-grow: 1; }\n          " +
         Glyph("kebab", TXM) + "        }\n";
  };
  auto chip = [&](const char* letter, const char* lcol, const std::string& valname) {
    s += "          panel { layout: row; align: center; gap: 4; flex-grow: 1; background: #0e1016;"
         " corner-radius: 6; padding: 5 6; border-color: #ffffff12; border-width: 1;\n"
         "            text { text: \"" + std::string(letter) + "\"; font-size: 11; color: " + lcol + "; }\n"
         "            text " + valname + " { text: \"0\"; font-size: 11; color: #d6dbe7; flex-grow: 1; }\n          }\n";
  };
  auto xyzrow = [&](const char* label, const std::string& px, const std::string& py,
                    const std::string& pz, bool lock) {
    s += "        panel { layout: row; align: center; gap: 8; width: 100%;\n"
         "          text { text: \"" + std::string(label) + "\"; font-size: 12; color: " + TXS + "; width: 56; }\n"
         "          panel { layout: row; align: center; gap: 5; flex-grow: 1;\n";
    chip("X", "#e5564b", px);
    chip("Y", "#57bd6a", py);
    chip("Z", "#4d8df0", pz);
    if (lock)
      s += "          panel { width: 18; height: 18; layout: column; justify: center; align: center; " +
           Glyph("lock", TXM) + "          }\n";
    s += "          }\n        }\n";
  };
  auto toggle = [&](const char* label, const std::string& name) {
    s += "        panel { layout: row; align: center; justify: space-between; width: 100%;\n"
         "          text { text: \"" + std::string(label) + "\"; font-size: 12; color: #c2c9d6; }\n"
         "          panel " + name + " { width: 34; height: 18; corner-radius: 9; background: " + AC + "; position: relative;\n"
         "            panel " + name + "_k { position: absolute; left: 18; top: 2; width: 14; height: 14; corner-radius: 7; background: #ffffff; }\n          }\n        }\n";
  };
  std::snprintf(buf, sizeof(buf),
                "\n    panel editor_inspector { position: absolute; right: 0; top: %g; width: %g;"
                " bottom: %g; layout: column; align: start; background: #171a22f8;"
                " border-color: #ffffff12; border-width: 1; overflow: hidden;\n",
                kEdToolbarH, kEdInspectorW, kEdStatusH);
  s += buf;
  s += "      panel { layout: row; align: center; justify: space-between; padding: 12 14; width: 100%;\n"
       "        text { text: \"Inspector\"; font-size: 13; color: #e6e9f2; letter-spacing: 1; }\n        " +
       Glyph("kebab", TXM) + "      }\n";
  s += "      panel { width: 100%; height: 1; background: #ffffff10; }\n";
  s += "      panel ed_insp_empty { layout: column; align: center; justify: center; padding: 40 0; width: 100%;\n"
       "        text { text: \"No object selected\"; font-size: 12; color: #6b7488; } }\n";
  s += "      panel ed_insp_body { layout: column; align: start; padding: 12 14; gap: 11; width: 100%;\n";
  // Object row.
  s += "        panel { layout: row; align: center; gap: 9; width: 100%;\n"
       "          panel { width: 26; height: 26; corner-radius: 6; background: #0e1016; layout: column; justify: center; align: center; " +
       Glyph("cube", TXS) + "          }\n"
       "          text ed_insp_name { text: \"\"; font-size: 15; color: #f2f4fb; flex-grow: 1; }\n"
       "          panel { layout: row; align: center; gap: 6;\n"
       "            text { text: \"Static\"; font-size: 11; color: #9aa3b5; }\n"
       "            panel ed_insp_static { width: 14; height: 14; corner-radius: 4; background: #0e1016; border-color: #ffffff24; border-width: 1; }\n          }\n        }\n";
  // Transform.
  section("Transform");
  s += "        panel { layout: column; gap: 7; width: 100%;\n";
  xyzrow("Position", "ed_pos_x", "ed_pos_y", "ed_pos_z", false);
  xyzrow("Rotation", "ed_rot_x", "ed_rot_y", "ed_rot_z", false);
  xyzrow("Scale", "ed_scl_x", "ed_scl_y", "ed_scl_z", true);
  s += "        }\n";
  // Model.
  section("Model");
  s += "        panel { layout: row; align: center; gap: 9; width: 100%;\n"
       "          panel { width: 40; height: 40; corner-radius: 7; background: #0e1016; overflow: hidden;"
       " image ed_model_thumb { width: 40; height: 40; } }\n"
       "          panel { layout: row; align: center; gap: 7; flex-grow: 1; background: #0e1016;"
       " corner-radius: 7; padding: 9 10; border-color: #ffffff14; border-width: 1;\n"
       "            text ed_model_name { text: \"\"; font-size: 12; color: #d6dbe7; flex-grow: 1; }\n            " +
       Glyph("folder", TXM) + "          }\n        }\n";
  s += "        panel { layout: row; align: center; gap: 9; width: 100%;\n"
       "          panel { width: 28; height: 28; corner-radius: 14; background: #0e1016; layout: column; justify: center; align: center; " +
       Glyph("sphere", TXS) + "          }\n"
       "          panel { layout: row; align: center; gap: 7; flex-grow: 1; background: #0e1016;"
       " corner-radius: 7; padding: 9 10; border-color: #ffffff14; border-width: 1;\n"
       "            text ed_mat_name { text: \"\"; font-size: 12; color: #d6dbe7; flex-grow: 1; }\n"
       "            text { text: \">\"; font-size: 12; color: #6b7488; }\n          }\n        }\n";
  // Details.
  section("Details");
  toggle("Cast Shadow", "ed_tg_cast");
  toggle("Receive Shadow", "ed_tg_recv");
  toggle("Lightmap Static", "ed_tg_lm");
  // Tags.
  section("Tags");
  s += "        panel ed_tags { layout: row; align: center; gap: 6; width: 100%;\n";
  for (int i = 0; i < kEdTags; ++i)
    s += "          panel ed_tag" + std::to_string(i) +
         " { background: #6c7bf52e; corner-radius: 6; padding: 5 9;"
         " text ed_tag" + std::to_string(i) + "_t { text: \"\"; font-size: 11; color: #b9c0ff; } }\n";
  s += "          panel { padding: 5; corner-radius: 6; background: #ffffff0c; cursor: pointer; :hover { background: #ffffff18; } " +
       Glyph("plus", TXM) + "          }\n";
  s += "        }\n";
  s += "      }\n    }\n";  // close ed_insp_body, editor_inspector

  // --- bottom dock: asset browser (width/left overridden each frame in C++) ---
  std::snprintf(buf, sizeof(buf),
                "\n    panel editor_browser { position: absolute; left: %g; bottom: %g; width: 1000;"
                " height: %g; layout: column; align: start; background: #171a22f8;"
                " border-color: #ffffff12; border-width: 1;\n",
                kEdSceneW, kEdStatusH, kEdBrowserH);
  s += buf;
  // Tab bar.
  s += "      panel { layout: row; align: center; justify: space-between; width: 100%; padding: 0 12; height: 40;\n"
       "        panel ed_btabs { layout: row; align: center; gap: 2;\n";
  for (int i = 0; i < kEdTabs; ++i)
    s += "          panel btn_btab" + std::to_string(i) +
         " { layout: column; align: center; gap: 6; padding: 9 9; cursor: pointer; background: #ffffff00;\n"
         "            text btn_btab" + std::to_string(i) + "_t { text: \"\"; font-size: 12; color: #9aa3b5; }\n"
         "            panel btn_btab" + std::to_string(i) + "_ul { width: 100%; height: 2; corner-radius: 1; background: #6c7bf500; }\n          }\n";
  s += "          panel { padding: 8; background: #ffffff00; cursor: pointer; :hover { background: #ffffff12; } " +
       Glyph("plus", TXM) + "          }\n        }\n";
  s += "        panel { layout: row; align: center; gap: 12;\n          " + Glyph("magnify", TXM);
  s += "          panel { width: 90; height: 4; corner-radius: 2; background: #2a2f3a; position: relative;\n"
       "            panel { position: absolute; left: 54; top: -3; width: 10; height: 10; corner-radius: 5; background: #6c7bf5; } }\n          " +
       Glyph("grid", TXS);
  s += "          panel ed_cardprev { padding: 5 8; background: #ffffff0c; corner-radius: 6; cursor: pointer; :hover { background: #ffffff18; } text { text: \"<\"; font-size: 13; color: #c2c9d6; } }\n";
  s += "          panel ed_cardnext { padding: 5 8; background: #ffffff0c; corner-radius: 6; cursor: pointer; :hover { background: #ffffff18; } text { text: \">\"; font-size: 13; color: #c2c9d6; } }\n          " +
       Glyph("kebab", TXM) + "        }\n      }\n";
  s += "      panel { width: 100%; height: 1; background: #ffffff10; }\n";
  // Body: category list + cards.
  s += "      panel { layout: row; align: start; width: 100%; flex-grow: 1;\n"
       "        panel { layout: column; align: start; gap: 2; width: 156; padding: 10 10;\n"
       "          panel ed_asset_search { layout: row; align: center; gap: 7; width: 100%; background: #0e1016;"
       " corner-radius: 7; padding: 6 9; border-color: #ffffff14; border-width: 1; cursor: text; margin: 0 0 6 0;\n            " +
       Glyph("magnify", TXM) +
       "            text ed_asset_search_text { text: \"Search props...\"; font-size: 11; color: #6b7488; flex-grow: 1; }\n"
       "            button ed_asset_clear { text: \"x\"; font-size: 11; color: #6b7488; background: #ffffff00; cursor: pointer; :hover { color: #e6e9f2; } }\n          }\n";
  for (int i = 0; i < kEdCatRows; ++i)
    s += "          panel cl_row" + std::to_string(i) +
         " { layout: row; align: center; justify: space-between; width: 100%; padding: 5 8;"
         " corner-radius: 6; background: #ffffff00; cursor: pointer; :hover { background: #ffffff0e; }\n"
         "            text cl_row" + std::to_string(i) + "_n { text: \"\"; font-size: 12; color: #c2c9d6; }\n"
         "            text cl_row" + std::to_string(i) + "_c { text: \"\"; font-size: 11; color: #6b7488; }\n          }\n";
  s += "        }\n        panel { width: 1; height: 100%; background: #ffffff0c; }\n";
  s += "        panel ed_cards { layout: row; align: start; gap: 12; flex-grow: 1; padding: 12 14; overflow: hidden;\n";
  for (int i = 0; i < kEdCards; ++i) {
    const std::string id = std::to_string(i);
    s += "          panel card" + id + " { layout: column; align: center; gap: 6; width: 86; cursor: pointer;\n"
         "            panel card" + id + "_box { width: 86; height: 86; corner-radius: 8; background: #1f232e;"
         " border-color: #ffffff14; border-width: 1; overflow: hidden; position: relative;\n"
         "              panel card" + id + "_sw { position: absolute; left: 18; top: 18; width: 50; height: 50; corner-radius: 8; background: #2a2f3a; }\n"
         "              image card" + id + "_img { position: absolute; left: 0; top: 0; width: 86; height: 86; }\n            }\n"
         "            text card" + id + "_name { text: \"\"; font-size: 11; color: #c2c9d6; text-align: center; width: 86; }\n          }\n";
  }
  s += "        }\n      }\n    }\n";  // close cards, body, editor_browser

  // --- status bar ---
  std::snprintf(buf, sizeof(buf),
                "\n    panel editor_status { position: absolute; left: 0; bottom: 0; width: 100vw;"
                " height: %g; layout: row; align: center; justify: space-between; padding: 0 14;"
                " background: #1b1e27f8; border-color: #ffffff12; border-width: 1;\n",
                kEdStatusH);
  s += buf;
  s += R"(      panel { layout: row; align: center; gap: 8;
        panel { width: 8; height: 8; corner-radius: 4; background: #46c463; }
        text ed_status_left { text: "Ready"; font-size: 12; color: #c2c9d6; }
      }
      panel { layout: row; align: center; gap: 18;
        panel { layout: row; align: center; gap: 7;
          text { text: "Grid"; font-size: 11; color: #6b7488; }
          panel ed_grid { layout: row; align: center; gap: 5; background: #0e1016; corner-radius: 6;
            padding: 4 8; border-color: #ffffff14; border-width: 1; cursor: pointer;
)";
  s += "            text ed_grid_t { text: \"1 m\"; font-size: 11; color: #c2c9d6; }\n            " +
       Glyph("caret", TXM) + "          }\n        }\n";
  s += R"(        panel { layout: row; align: center; gap: 7;
          text { text: "Snapping"; font-size: 11; color: #6b7488; }
          panel ed_snap { width: 32; height: 16; corner-radius: 8; background: #2a2f3a; position: relative; cursor: pointer;
            panel ed_snap_k { position: absolute; left: 2; top: 2; width: 12; height: 12; corner-radius: 6; background: #cfd6e6; } }
        }
        text ed_fps { text: "fps 60"; font-size: 11; color: #9aa3b5; }
      }
    }
  }
)";
  return s;
}

// Menu emblems, composed from primitives inside a relative box of `sz` px (the
// UI font has no symbol set, so the NEXUS marks are built from filled and
// outlined rects, like the editor glyphs but scalable). `c` is the mark colour.
std::string MenuGlyph(const std::string& k, const char* c, float sz) {
  std::string body;
  char b[256];
  const float u = sz / 18.0f;  // authored on an 18-unit grid, scaled to sz
  // Sub-pixel panels confuse the layout (a near-zero height blows up to fill the
  // parent), so every mark is clamped to at least 1px on each side.
  auto R = [&](float x, float y, float w, float h, const char* cc, float r = 0) {
    std::snprintf(b, sizeof(b),
                  "panel { position: absolute; left: %g; top: %g; width: %g; height: %g;"
                  " background: %s; corner-radius: %g; }\n",
                  x * u, y * u, std::max(w * u, 1.0f), std::max(h * u, 1.0f), cc, r * u);
    body += b;
  };
  auto O = [&](float x, float y, float w, float h, const char* cc, float bw, float r) {
    std::snprintf(b, sizeof(b),
                  "panel { position: absolute; left: %g; top: %g; width: %g; height: %g;"
                  " border-color: %s; border-width: %g; corner-radius: %g; }\n",
                  x * u, y * u, std::max(w * u, 1.0f), std::max(h * u, 1.0f), cc, bw, r * u);
    body += b;
  };
  // A filled triangle (pyramid of rows) pointing up, base at y+h.
  auto Tri = [&](float cx, float y, float baseHalf, float h, const char* cc) {
    const int rows = 7;
    for (int i = 0; i < rows; ++i) {
      float t = static_cast<float>(i) / (rows - 1);
      float w = baseHalf * 2.0f * t;
      if (w < 1.0f) w = 1.0f;
      R(cx - w / 2.0f, y + h * t, w, h / rows + 0.7f, cc);
    }
  };
  // A vertical diamond/lozenge centred at (cx,cy); halfH tall, halfW wide.
  auto Loz = [&](float cx, float cy, float halfW, float halfH, const char* cc) {
    const int rows = 9;
    for (int i = 0; i < rows; ++i) {
      float t = static_cast<float>(i) / (rows - 1);
      float taper = 1.0f - std::fabs(t - 0.5f) * 2.0f;
      float w = halfW * 2.0f * taper;
      if (w < 0.8f) w = 0.8f;
      R(cx - w / 2.0f, cy - halfH + 2.0f * halfH * t, w, 2.0f * halfH / rows + 0.6f, cc);
    }
  };

  if (k == "mountain") {  // top-left logo: twin snow peaks
    Tri(7, 4, 5.5f, 11, c);
    Tri(12, 7, 4, 8, c);
  } else if (k == "skyrim") {  // a tall vertical diamond, dragon-mark shorthand
    Loz(9, 9, 3.0f, 8.0f, c);
    R(8, 7, 2, 6, "#00000055");
  } else if (k == "vault") {  // Fallout: cog ring with ticks
    O(2, 2, 14, 14, c, 1.8f, 7);
    R(8, 0, 2, 3, c);
    R(8, 15, 2, 3, c);
    R(0, 8, 3, 2, c);
    R(15, 8, 3, 2, c);
    R(7, 7, 4, 4, c, 2);
  } else if (k == "constellation") {  // Starfield: ringed dot with orbit ticks
    O(3, 3, 12, 12, c, 1.6f, 6);
    R(7, 7, 4, 4, c, 2);
    R(14, 2, 2, 2, c, 1);
    R(2, 13, 2, 2, c, 1);
  } else if (k == "knot") {  // profile: concentric celtic rings
    O(1, 1, 16, 16, c, 1.5f, 8);
    O(4, 4, 10, 10, c, 1.3f, 5);
    R(8, 1, 2, 16, c);
    R(1, 8, 16, 2, c);
    R(7, 7, 4, 4, c, 2);
  } else if (k == "people") {  // players-online
    R(2, 8, 6, 7, c, 3);
    R(3, 4, 4, 4, c, 2);
    R(10, 8, 6, 7, c, 3);
    R(11, 4, 4, 4, c, 2);
  } else if (k == "globe") {
    O(2, 2, 14, 14, c, 1.4f, 7);
    R(8, 2, 1.4f, 14, c);
    R(2, 8, 14, 1.4f, c);
    O(5, 2, 8, 14, c, 1.1f, 6);
  } else if (k == "discord") {
    R(3, 5, 12, 8, c, 4);
    R(4, 12, 3, 3, c, 1);
    R(11, 12, 3, 3, c, 1);
    R(6, 8, 2, 2, "#00000088", 1);
    R(10, 8, 2, 2, "#00000088", 1);
  } else if (k == "news") {
    O(2, 3, 14, 12, c, 1.3f, 2);
    R(4, 6, 7, 1.6f, c);
    R(4, 9, 10, 1.4f, c);
    R(4, 11, 10, 1.4f, c);
  } else if (k == "gear") {
    O(4, 4, 10, 10, c, 1.8f, 5);
    R(8, 1, 2, 3, c);
    R(8, 14, 2, 3, c);
    R(1, 8, 3, 2, c);
    R(14, 8, 3, 2, c);
    R(2, 3, 2, 2, c, 1);
    R(14, 3, 2, 2, c, 1);
  } else if (k == "caret") {  // PLAY's active ► (points right)
    R(5, 4, 2, 10, c);
    R(7, 6, 2, 6, c);
    R(9, 8, 2, 2, c);
  } else if (k == "triup") {  // bottom-centre up-caret
    Tri(9, 6, 6, 7, c);
  } else if (k == "diamond") {
    Loz(9, 9, 4, 4, c);
  } else if (k == "sparkle") {  // the NEXUS centre mark: a 4-point star
    Loz(9, 9, 1.4f, 9, c);
    Loz(9, 9, 9, 1.4f, c);
    R(7, 7, 4, 4, c, 2);
  }
  std::snprintf(b, sizeof(b), "panel { position: relative; width: %g; height: %g; ", sz, sz);
  return std::string(b) + body + " }\n";
}

// The main menu sub-screens (multiplayer / mods / settings / profile): one
// dimmed overlay with a centred card whose title and body swap by which nav
// item opened it. Collapsed until a nav item is picked; ApplyMainMenu shows the
// matching body. Built as a helper so BuildMainMenuSection stays readable.
std::string BuildMainMenuScreens() {
  char buf[512];
  std::string s = R"(
    panel mm_screen { position: absolute; left: 0; top: 0; width: 100vw; height: 100vh;
      layout: column; justify: center; align: center; background: #04060bee; visibility: collapsed;
      panel mm_card { layout: column; align: start; padding: 34 44; width: 640;
        background: #0b0e16fa; corner-radius: 16; border-color: #ffffff1c; border-width: 1;
        shadow-color: #000000aa; shadow-blur: 52; shadow-y: 20;
        panel { layout: row; align: center; justify: space-between; width: 100%; margin: 0 0 16 0;
          text mm_screen_title { text: "MULTIPLAYER"; font-size: 25; color: #f2f4fb;
            letter-spacing: 8; text-transform: uppercase; }
          panel mm_back { cursor: pointer; padding: 7 14; corner-radius: 8; background: #ffffff12;
            border-color: #ffffff1c; border-width: 1; :hover { background: #ffffff22; }
            text { text: "Back"; font-size: 14; color: #cfd6e6; } }
        }
        panel { width: 100%; height: 1; background: #ffffff18; margin: 0 0 18 0; }
)";

  // --- Multiplayer ---
  s += R"(        panel mm_body_mp { layout: column; gap: 12; width: 100%;
          text { text: "SELECTED UNIVERSE"; font-size: 11; color: #8a93a8; letter-spacing: 3; }
          text mm_mp_universe { text: "Skyrim"; font-size: 19; color: #ffcc55; letter-spacing: 1; }
          panel { layout: row; gap: 12; width: 100%; margin: 6 0 0 0;
            panel mm_mp_host { flex-grow: 1; cursor: pointer; layout: column; gap: 4; padding: 15 16;
              corner-radius: 10; background: #ffcc5522; border-color: #ffcc5544; border-width: 1;
              :hover { background: #ffcc5538; }
              text { text: "HOST SERVER"; font-size: 15; color: #ffe6a0; letter-spacing: 2; }
              text { text: "Start a session others can join"; font-size: 12; color: #c9b894; }
            }
            panel mm_mp_join { flex-grow: 1; cursor: pointer; layout: column; gap: 4; padding: 15 16;
              corner-radius: 10; background: #ffffff0c; border-color: #ffffff1c; border-width: 1;
              :hover { background: #ffffff18; }
              text { text: "JOIN SERVER"; font-size: 15; color: #e8ecf6; letter-spacing: 2; }
              text mm_mp_addr { text: "127.0.0.1:29700"; font-size: 12; color: #9aa3b6; }
            }
          }
          text mm_mp_status { text: "Offline"; font-size: 12; color: #8a93a8; margin: 6 0 0 0; }
          text { text: "The session opens as you enter the world."; font-size: 11; color: #6b7488; }
        }
)";

  // --- Mods ---
  s += R"(        panel mm_body_mods { layout: column; gap: 9; width: 100%;
          text mm_mods_head { text: "C# GAMEPLAY MODULES"; font-size: 11; color: #8a93a8; letter-spacing: 3; }
          panel mm_mods_list { layout: column; gap: 5; width: 100%;
)";
  for (int i = 0; i < kMenuModRows; ++i) {
    const std::string id = std::to_string(i);
    s += "            panel mm_mod" + id +
         " { layout: row; align: center; gap: 11; width: 100%; padding: 6 9; corner-radius: 7;"
         " background: #ffffff06;\n"
         "              panel { width: 7; height: 7; corner-radius: 4; background: #46c463; }\n"
         "              text mm_modt" + id +
         " { text: \"\"; font-size: 13; color: #d6dbe7; flex-grow: 1; }\n            }\n";
  }
  s += R"(          }
          text mm_mods_empty { text: "Enter a universe to load its gameplay modules."; font-size: 12; color: #6b7488; }
        }
)";

  // --- Settings (controls reference) ---
  s += R"(        panel mm_body_settings { layout: column; gap: 5; width: 100%;
          text { text: "CONTROLS"; font-size: 11; color: #8a93a8; letter-spacing: 3; margin: 0 0 4 0; }
)";
  const char* keybinds[][2] = {
      {"WASD", "Move"},        {"Mouse", "Look"},          {"Shift", "Sprint"},
      {"E", "Activate / talk"}, {"T", "Toggle walk / fly"}, {"J", "Journal"},
      {"F3", "Quest debugger"}, {"F4", "Map editor"},       {"Esc", "Pause menu"},
  };
  for (const auto& kb : keybinds) {
    s += "          panel { layout: row; justify: space-between; align: center; width: 100%; padding: 5 0;\n"
         "            text { text: \"" + std::string(kb[1]) +
         "\"; font-size: 14; color: #d8def0; }\n"
         "            text { text: \"" + std::string(kb[0]) +
         "\"; font-size: 14; color: #9aa2b6; letter-spacing: 1; }\n          }\n";
  }
  s += "        }\n";

  // --- Profile (live stats) ---
  s += "        panel mm_body_profile { layout: column; gap: 14; width: 100%;\n"
       "          panel { layout: row; align: center; gap: 16; width: 100%;\n            " +
       MenuGlyph("knot", "#cdb98f", 40) +
       R"(            panel { layout: column; gap: 2;
              text mm_pf_name { text: "Wanderer"; font-size: 20; color: #eef1f8; }
              text mm_pf_sub { text: "Level 42"; font-size: 12; color: #9aa3b6; }
            }
          }
          panel { layout: column; gap: 9; width: 100%;
)";
  struct Bar {
    const char* label;
    const char* track;
    const char* fill;
    const char* a;
    const char* b;
  };
  const Bar bars[3] = {
      {"Health", "mm_pf_htrack", "mm_pf_health", "#b23a3a", "#e8645d"},
      {"Magicka", "mm_pf_mtrack", "mm_pf_magicka", "#3a63b2", "#5d92e8"},
      {"Stamina", "mm_pf_strack", "mm_pf_stamina", "#3aa05a", "#5de88a"},
  };
  for (const Bar& bar : bars) {
    std::snprintf(buf, sizeof(buf),
                  "            panel { layout: row; align: center; gap: 12; width: 100%%;\n"
                  "              text { text: \"%s\"; font-size: 12; color: #9aa3b6; width: 66; }\n"
                  "              panel %s { flex-grow: 1; height: 12; overflow: hidden; background: #0c0e16cc;"
                  " corner-radius: 6; border-color: #00000066; border-width: 1;\n"
                  "                panel %s { width: 100%%; height: 100%%; corner-radius: 6;"
                  " background: %s; background-end: %s; }\n              }\n            }\n",
                  bar.label, bar.track, bar.fill, bar.a, bar.b);
    s += buf;
  }
  s += R"(          }
          panel { layout: row; gap: 10; width: 100%; margin: 4 0 0 0;
)";
  const char* chips[3][2] = {{"GOLD", "mm_pf_gold"}, {"QUESTS", "mm_pf_quests"}, {"LOCATION", "mm_pf_loc"}};
  for (auto& chip : chips) {
    s += "            panel { flex-grow: 1; layout: column; gap: 3; padding: 10 12; corner-radius: 9;"
         " background: #ffffff0a; border-color: #ffffff14; border-width: 1;\n"
         "              text { text: \"" + std::string(chip[0]) +
         "\"; font-size: 10; color: #8a93a8; letter-spacing: 2; }\n"
         "              text " + std::string(chip[1]) +
         " { text: \"-\"; font-size: 15; color: #eef1f8; }\n            }\n";
  }
  s += R"(          }
          text mm_pf_hint { text: "Enter a universe to begin your story."; font-size: 11; color: #6b7488; }
        }
      }
    }
)";
  return s;
}

// The NEXUS main menu: the startup "choose your universe" screen. Three column
// backdrops (each a live/captured game scene over a themed gradient), the brand
// wordmark, the left navigation, a news block, the player banner and the social
// row, plus the sub-screens for multiplayer / mods / settings / profile. Every
// stateful element is driven each frame from ApplyMainMenu; starts collapsed.
std::string BuildMainMenuSection() {
  std::string s;

  // Per-universe theme: backdrop gradient stops + accent + label + emblem.
  struct Col {
    const char* stops;
    const char* accent;
    const char* label;
    const char* emblem;
  };
  const Col cols[kMenuUniverses] = {
      {"#28323f 0%, #38485c 38%, #141b26 78%, #0a0e16 100%", "#9fb4d6", "SKYRIM", "skyrim"},
      {"#2c2516 0%, #5a4a2c 34%, #2a2014 72%, #0f0b07 100%", "#d8b173", "FALLOUT 4", "vault"},
      {"#0c1530 0%, #241f48 36%, #11132c 74%, #05060f 100%", "#8ea2e8", "STARFIELD", "constellation"},
  };

  s += R"(
  panel mainmenu {
    position: absolute; top: 0; left: 0; width: 100vw; height: 100vh; background: #04060b;
    panel mm_cols { position: absolute; top: 0; left: 0; width: 100vw; height: 100vh; layout: row;
)";
  for (int i = 0; i < kMenuUniverses; ++i) {
    const Col& c = cols[i];
    const std::string id = std::to_string(i);
    // Column: themed gradient base, the live backdrop image over it, a soft top
    // and a strong bottom vignette for legibility, a top selection accent, and
    // the universe label + emblem near the foot. Built by concatenation (not a
    // fixed snprintf buffer) so the multi-line emblem glyph never truncates.
    s += "      panel mm_col" + id +
         " { flex-grow: 1; height: 100%; position: relative; overflow: hidden; cursor: pointer;\n"
         "        panel mm_grad" + id +
         " { position: absolute; left: 0; right: 0; top: 0; bottom: 0; background: #000000;"
         " gradient-stops: " + c.stops + "; gradient-angle: 180; }\n"
         "        image mm_bg" + id +
         " { position: absolute; left: 0; right: 0; top: 0; bottom: 0; }\n"
         "        panel mm_vtop" + id +
         " { position: absolute; left: 0; top: 0; width: 100%; height: 200;"
         " background: #05070ecc; background-end: #05070e00; gradient-angle: 180; }\n"
         "        panel mm_vbot" + id +
         " { position: absolute; left: 0; bottom: 0; width: 100%; height: 360;"
         " background: #04060b00; background-end: #04060bf2; gradient-angle: 180; }\n"
         "        panel mm_sel" + id +
         " { position: absolute; left: 0; top: 0; width: 100%; height: 3; background: " + c.accent +
         "; visibility: collapsed; }\n"
         "        panel mm_lab" + id +
         " { position: absolute; left: 0; bottom: 150; width: 100%; layout: column; align: center;"
         " gap: 14;\n          text mm_labt" + id + " { text: \"" + c.label +
         "\"; font-size: 17; color: #dfe4ef; letter-spacing: 7; text-transform: uppercase;"
         " text-shadow-color: #000000c0; text-shadow-x: 1; text-shadow-y: 1; }\n          " +
         MenuGlyph(c.emblem, "#cfd6e6", 26) + "        }\n      }\n";
  }
  s += "    }\n";  // close mm_cols

  // Thin seams between the thirds.
  s += "    panel mm_seaml { position: absolute; left: 33.333%; top: 0; width: 1; height: 100vh;"
       " background: #00000066; }\n";
  s += "    panel mm_seamr { position: absolute; left: 66.666%; top: 0; width: 1; height: 100vh;"
       " background: #00000066; }\n";

  // Top-left brand lockup.
  s += "    panel mm_logo { position: absolute; left: 40; top: 34; layout: row; align: center; gap: 12;\n      " +
       MenuGlyph("mountain", "#dfe4ef", 22) +
       "      text { text: \"NEXUS\"; font-size: 15; color: #e8ecf6; letter-spacing: 6; }\n    }\n";

  // Centre wordmark: N E + star + U S, tagline, ornament.
  s += R"(    panel mm_word { position: absolute; left: 0; top: 70; width: 100vw;
      layout: column; align: center; gap: 9;
      panel { layout: row; align: center; gap: 0;
        text { text: "NE"; font-size: 60; color: #f4f6fb; letter-spacing: 20;
          text-shadow-color: #000000aa; text-shadow-x: 0; text-shadow-y: 2; }
)";
  s += "        " + MenuGlyph("sparkle", "#eaf0ff", 60) +
       R"(        text { text: "XUS"; font-size: 60; color: #f4f6fb; letter-spacing: 20;
          text-shadow-color: #000000aa; text-shadow-x: 0; text-shadow-y: 2; }
      }
      text { text: "ONE UNIVERSE. INFINITE STORIES."; font-size: 13; color: #aeb6c8;
        letter-spacing: 6; text-transform: uppercase; }
)";
  s += "      " + MenuGlyph("diamond", "#8a93a8", 14) + "    }\n";

  // Left navigation.
  const char* nav[kMenuNavItems][2] = {
      {"PLAY", "Choose your universe"},     {"MULTIPLAYER", "Join or host a server"},
      {"MODS", "Browse and manage mods"},   {"SETTINGS", "Configure your experience"},
      {"PROFILE", "View stats and progress"}, {"QUIT", "Exit to desktop"},
  };
  s += "    panel mm_nav { position: absolute; left: 44; top: 188; width: 360; layout: column; gap: 24;\n";
  for (int i = 0; i < kMenuNavItems; ++i) {
    const std::string id = std::to_string(i);
    s += "      panel mm_nav" + id +
         " { layout: row; align: center; gap: 14; padding: 4 8 4 0; cursor: pointer;\n"
         "        panel mm_caret" + id + " { width: 12; height: 16; visibility: collapsed; " +
         MenuGlyph("caret", "#ffcc55", 16) +
         "        }\n"
         "        panel { layout: column; gap: 2;\n"
         "          text mm_navt" + id + " { text: \"" + nav[i][0] +
         "\"; font-size: 23; color: #9aa3b6; letter-spacing: 4;"
         " text-shadow-color: #000000b0; text-shadow-x: 1; text-shadow-y: 1; }\n"
         "          text mm_navs" + id + " { text: \"" + nav[i][1] +
         "\"; font-size: 12; color: #6b7488;"
         " text-shadow-color: #000000a0; text-shadow-x: 1; text-shadow-y: 1; }\n"
         "        }\n      }\n";
  }
  s += "    }\n";

  // Right-hand news block.
  s += R"(    panel mm_news { position: absolute; right: 48; top: 150; width: 188; layout: column; align: start; gap: 10;
      text { text: "NEWS"; font-size: 12; color: #aeb6c8; letter-spacing: 5; }
      panel { layout: row; align: start; gap: 9;
        panel { width: 4; height: 4; corner-radius: 2; background: #ffcc55; margin: 6 0 0 0; }
        panel { layout: column; gap: 3;
          text { text: "CREATION KIT 2 IS LIVE"; font-size: 13; color: #eef1f8; letter-spacing: 1; }
          text { text: "Build, share, play."; font-size: 13; color: #aeb6c8; }
          text { text: "Together."; font-size: 13; color: #aeb6c8; }
        }
      }
      panel mm_news_view { layout: column; gap: 4; cursor: pointer; margin: 6 0 0 0;
        text { text: "VIEW"; font-size: 11; color: #cfd6e6; letter-spacing: 3; }
        panel { width: 26; height: 1; background: #cfd6e6; }
      }
    }
)";

  // Bottom-left player banner.
  s += "    panel mm_profilebar { position: absolute; left: 40; bottom: 40; layout: row; align: center; gap: 16;\n";
  s += "      " + MenuGlyph("knot", "#cdb98f", 46);
  s += R"(      panel { layout: column; gap: 1;
        text mm_pname { text: "Wanderer"; font-size: 18; color: #eef1f8; letter-spacing: 1; }
        text mm_plevel { text: "Level 42"; font-size: 12; color: #9aa3b6; letter-spacing: 1; }
      }
      panel { width: 1; height: 34; background: #ffffff24; margin: 0 4; }
      panel { layout: row; align: center; gap: 8;
)";
  s += "        " + MenuGlyph("people", "#9aa3b6", 20) +
       "        text mm_pcount { text: \"3\"; font-size: 15; color: #cfd6e6; }\n      }\n    }\n";

  // Bottom-centre tagline.
  s += "    panel mm_foot { position: absolute; left: 0; bottom: 30; width: 100vw; layout: column; align: center; gap: 8;\n      " +
       MenuGlyph("triup", "#8a93a8", 12) +
       "      text { text: \"EXPLORE. SURVIVE. BUILD. TOGETHER.\"; font-size: 11; color: #8a93a8;"
       " letter-spacing: 5; }\n    }\n";

  // Bottom-right social row.
  s += "    panel mm_icons { position: absolute; right: 40; bottom: 38; layout: row; align: center; gap: 22;\n";
  const char* icons[4] = {"globe", "discord", "news", "gear"};
  for (int i = 0; i < 4; ++i)
    s += "      panel mm_icon" + std::to_string(i) +
         " { cursor: pointer; " + MenuGlyph(icons[i], "#9aa3b6", 22) + "      }\n";
  s += "    }\n";

  s += BuildMainMenuScreens();
  s += "  }\n";  // close mainmenu
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
)";
  // The NEXUS startup menu draws last, on top of everything (it is the front
  // screen). It is a sibling of the pause menu inside root.
  s += BuildMainMenuSection();
  s += "}\n";
  return s;
}

const char* FindFont() {
  static std::string resolved;
  if (const char* env = std::getenv("RECREATION_UI_FONT"); env && fs::exists(env)) {
    resolved = env;
    return resolved.c_str();
  }
#if !defined(_WIN32)
  // fontconfig's fc-match is the Linux/BSD source of truth; Windows has no such
  // tool, so it falls straight through to the candidate list below.
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
#endif
  static const char* candidates[] = {
#if defined(_WIN32)
      "C:/Windows/Fonts/segoeui.ttf",
      "C:/Windows/Fonts/arial.ttf",
#else
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/usr/share/fonts/noto/NotoSans-Regular.ttf",
      "/run/current-system/sw/share/X11/fonts/DejaVuSans.ttf",
#endif
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
  int last_fps = 0;  // last computed fps, shown in the editor status bar

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

  // NEXUS main menu state, driven by the engine and the click router. The
  // request is raised here (click / keyboard) and consumed by PollMainMenuRequest.
  bool main_menu_open = false;
  int mm_universe = 1;  // selected column: 0 Skyrim, 1 Fallout 4, 2 Starfield (centre)
  int mm_nav = 0;       // highlighted left-nav row (0 PLAY .. 5 QUIT)
  int mm_screen = 0;    // 0 root, 1 multiplayer, 2 mods, 3 settings, 4 profile
  int mm_mp_mode = 0;   // last multiplayer choice: 0 host, 1 join
  MainMenuRequest mm_request;
  MainMenuStats mm_stats;
  std::vector<std::string> mm_universe_names{"Skyrim", "Fallout 4", "Starfield"};
  std::vector<bool> mm_available{true, true, true};
  std::vector<std::string> mm_mods;
  u64 mm_backdrop[kMenuUniverses] = {0, 0, 0};
  bool mm_prev_open = false;  // edge-detect to hide the gameplay HUD while open

  // Drives every main-menu widget from the state above each frame; collapses the
  // whole overlay when closed. Activate runs the highlighted nav item.
  void ApplyMainMenu();
  void ActivateNav();
  // Climbs from a clicked widget to the nearest menu-handled name and acts on it.
  // Returns true if it consumed the click.
  bool RouteMainMenuClick(ugui::wid target);

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
inline ugui::Color Rgba(u32 hex) {
  return ugui::Color::FromRgba8((hex >> 24) & 0xff, (hex >> 16) & 0xff, (hex >> 8) & 0xff,
                                hex & 0xff);
}
const ugui::Color kEdAccent = Rgba(0x6c7bf5ff);      // indigo accent
const ugui::Color kEdAccentSoft = Rgba(0x6c7bf52e);  // selected-row wash
const ugui::Color kEdClear = Rgba(0x00000000);
const ugui::Color kEdTxP = Rgba(0xe6e9f2ff);  // primary text
const ugui::Color kEdTxS = Rgba(0x9aa3b5ff);  // secondary text
const ugui::Color kEdTxM = Rgba(0x6b7488ff);  // muted text
const ugui::Color kEdLeaf = Rgba(0xd6dbe7ff);
const ugui::Color kEdField = Rgba(0x0e1016ff);
const ugui::Color kEdEyeOn = Rgba(0xc8cfddff);
const ugui::Color kEdEyeOff = Rgba(0x4a505fff);
const ugui::Color kEdToggleOff = Rgba(0x2a2f3aff);
const ugui::Color kEdIcoGroup = Rgba(0x8a93a8ff);
const ugui::Color kEdIcoMesh = Rgba(0x4d8df0ff);
const ugui::Color kEdIcoLight = Rgba(0xe8b54aff);
const ugui::Color kEdCardBorder = Rgba(0xffffff14);
const ugui::Color kEdCat = Rgba(0xc2c9d6ff);
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

  auto setText = [&](const std::string& n, const std::string& t) {
    ugui::SetText(ui.FindWidget(n.c_str()), t.c_str());
  };
  auto setLeft = [&](const std::string& n, float v) {
    SetStyleField(
        n.c_str(), [](ugui::Style& s, float v) { s.left_offset = ugui::Length::Px(v); }, v);
  };
  auto setWidth = [&](const std::string& n, float v) {
    SetStyleField(
        n.c_str(), [](ugui::Style& s, float v) { s.width = ugui::Length::Px(v); }, v);
  };

  // Keep the bottom browser spanning between the side docks at any window width.
  const float bw = host.window_width - kEdSceneW - kEdInspectorW;
  setLeft("editor_browser", kEdSceneW);
  setWidth("editor_browser", bw > 200.0f ? bw : 200.0f);

  // Toolbar: highlight the active tool; gizmo bar mirrors the gizmo mode.
  for (int i = 0; i < kEdToolBtns; ++i) {
    const std::string id = "btn_tool" + std::to_string(i);
    const bool on = i == editor.tool;
    SetBackground(id.c_str(), on ? kEdAccent : kEdClear);
    SetTextColor((id + "_lbl").c_str(), on ? kEdTxP : kEdTxS);
  }
  for (int i = 0; i < 4; ++i)
    SetBackground(("btn_giz" + std::to_string(i)).c_str(), i == editor.gizmo ? kEdAccent : kEdClear);

  // Left dock tabs.
  SetTextColor("btn_ltab0_t", editor.left_tab == 0 ? kEdTxP : kEdTxM);
  SetTextColor("btn_ltab1_t", editor.left_tab == 1 ? kEdTxP : kEdTxM);
  SetBackground("btn_ltab0_ul", editor.left_tab == 0 ? kEdAccent : kEdClear);
  SetBackground("btn_ltab1_ul", editor.left_tab == 1 ? kEdAccent : kEdClear);

  // Scene tree search box.
  {
    const bool ph = editor.scene_search.empty() && !editor.scene_search_focused;
    setText("ed_scene_search_text",
            ph ? "Search scene..."
               : editor.scene_search + (editor.scene_search_focused ? "|" : ""));
    SetTextColor("ed_scene_search_text", ph ? kEdTxM : kEdTxP);
  }

  // Scene hierarchy tree rows.
  for (int i = 0; i < kEdTreeRows; ++i) {
    const std::string row = "ed_trow" + std::to_string(i);
    if (i < static_cast<int>(editor.tree.size())) {
      const EditorView::TreeRow& tr = editor.tree[i];
      SetVisible(row.c_str(), true);
      setText(row + "_name", tr.name);
      setWidth(row + "_pad", 2.0f + tr.depth * 14.0f);
      setText(row + "_exp", tr.expand == 1 ? "+" : tr.expand == 2 ? "-" : " ");
      SetBackground(row.c_str(), tr.selected ? kEdAccentSoft : kEdClear);
      SetTextColor((row + "_name").c_str(), tr.selected ? kEdTxP : kEdLeaf);
      const ugui::Color ic = tr.icon == 0   ? kEdAccent
                             : tr.icon == 1 ? kEdIcoGroup
                             : tr.icon == 2 ? kEdIcoLight
                                            : kEdIcoMesh;
      SetBackground((row + "_ico").c_str(), ic);
      SetBackground((row + "_eye").c_str(), tr.hidden ? kEdEyeOff : kEdEyeOn);
    } else {
      SetVisible(row.c_str(), false);
    }
  }

  // Inspector: live selection or the empty state.
  SetVisible("ed_insp_empty", !editor.has_selection);
  SetVisible("ed_insp_body", editor.has_selection);
  if (editor.has_selection) {
    setText("ed_insp_name", editor.sel_name);
    char b[48];
    const char* pn[3] = {"ed_pos_x", "ed_pos_y", "ed_pos_z"};
    const char* rn[3] = {"ed_rot_x", "ed_rot_y", "ed_rot_z"};
    const char* sn[3] = {"ed_scl_x", "ed_scl_y", "ed_scl_z"};
    for (int a = 0; a < 3; ++a) {
      std::snprintf(b, sizeof(b), "%.1f", editor.pos[a]);
      setText(pn[a], b);
      std::snprintf(b, sizeof(b), "%.0f", editor.rot[a]);
      setText(rn[a], b);
      std::snprintf(b, sizeof(b), "%.2f", editor.scale[a]);
      setText(sn[a], b);
    }
    setText("ed_model_name", editor.model_name);
    setText("ed_mat_name", editor.material_name);
    ugui::SetImageTexture(ui.FindWidget("ed_model_thumb"), editor.model_thumb,
                          editor.model_thumb ? 40.0f : 0.0f, editor.model_thumb ? 40.0f : 0.0f);
    SetBackground("ed_insp_static", editor.sel_static ? kEdAccent : kEdField);
    auto toggle = [&](const char* name, bool on) {
      SetBackground(name, on ? kEdAccent : kEdToggleOff);
      setLeft(std::string(name) + "_k", on ? 18.0f : 2.0f);
    };
    toggle("ed_tg_cast", editor.cast_shadow);
    toggle("ed_tg_recv", editor.receive_shadow);
    toggle("ed_tg_lm", editor.lightmap_static);
    for (int i = 0; i < kEdTags; ++i) {
      const std::string t = "ed_tag" + std::to_string(i);
      if (i < static_cast<int>(editor.tags.size())) {
        SetVisible(t.c_str(), true);
        setText(t + "_t", editor.tags[i]);
      } else {
        SetVisible(t.c_str(), false);
      }
    }
  }

  // Asset-browser tabs.
  for (int i = 0; i < kEdTabs; ++i) {
    const std::string id = "btn_btab" + std::to_string(i);
    if (i < static_cast<int>(editor.tabs.size())) {
      SetVisible(id.c_str(), true);
      setText(id + "_t", editor.tabs[i]);
      const bool on = i == editor.tab;
      SetTextColor((id + "_t").c_str(), on ? kEdTxP : kEdTxS);
      SetBackground((id + "_ul").c_str(), on ? kEdAccent : kEdClear);
    } else {
      SetVisible(id.c_str(), false);
    }
  }

  // Asset-browser search box.
  {
    const bool ph = editor.asset_search.empty() && !editor.asset_search_focused;
    setText("ed_asset_search_text",
            ph ? "Search props..."
               : editor.asset_search + (editor.asset_search_focused ? "|" : ""));
    SetTextColor("ed_asset_search_text", ph ? kEdTxM : kEdTxP);
  }

  // Category list with counts.
  for (int i = 0; i < kEdCatRows; ++i) {
    const std::string id = "cl_row" + std::to_string(i);
    if (i < static_cast<int>(editor.cats.size())) {
      SetVisible(id.c_str(), true);
      setText(id + "_n", editor.cats[i].name);
      setText(id + "_c", std::to_string(editor.cats[i].count));
      const bool on = editor.cats[i].active;
      SetBackground(id.c_str(), on ? kEdAccentSoft : kEdClear);
      SetTextColor((id + "_n").c_str(), on ? kEdTxP : kEdCat);
    } else {
      SetVisible(id.c_str(), false);
    }
  }

  // Asset cards.
  for (int i = 0; i < kEdCards; ++i) {
    const std::string id = "card" + std::to_string(i);
    if (i < static_cast<int>(editor.cards.size())) {
      const EditorView::Card& cd = editor.cards[i];
      SetVisible(id.c_str(), true);
      setText(id + "_name", cd.name);
      SetBackground((id + "_sw").c_str(), Rgba(cd.color ? cd.color : 0x2a2f3aff));
      ugui::wid img = ui.FindWidget((id + "_img").c_str());
      if (cd.thumb) {
        ugui::SetImageTexture(img, cd.thumb, 86.0f, 86.0f);
        SetVisible((id + "_sw").c_str(), false);
      } else {
        ugui::SetImageTexture(img, 0, 0.0f, 0.0f);
        SetVisible((id + "_sw").c_str(), true);
      }
      // Armed/selected card: indigo border on the thumbnail box.
      ugui::wid box = ui.FindWidget((id + "_box").c_str());
      if (box.valid()) {
        if (ugui::StyleC* sc = ui.world().Get<ugui::StyleC>(box)) {
          ugui::Style st = sc->style;
          st.border_color = cd.armed ? kEdAccent : kEdCardBorder;
          st.border_width = cd.armed ? 2.0f : 1.0f;
          ugui::SetStyle(ui.world(), box, st);
        }
      }
    } else {
      SetVisible(id.c_str(), false);
    }
  }

  // Marquee box-select rectangle.
  SetVisible("ed_marquee", editor.marquee_active);
  if (editor.marquee_active) {
    const float x0 = std::min(editor.marquee[0], editor.marquee[2]);
    const float y0 = std::min(editor.marquee[1], editor.marquee[3]);
    const float w = std::fabs(editor.marquee[2] - editor.marquee[0]);
    const float h = std::fabs(editor.marquee[3] - editor.marquee[1]);
    setLeft("ed_marquee", x0);
    SetStyleField("ed_marquee", [](ugui::Style& s, float v) { s.top = ugui::Length::Px(v); }, y0);
    setWidth("ed_marquee", w);
    SetStyleField("ed_marquee", [](ugui::Style& s, float v) { s.height = ugui::Length::Px(v); }, h);
  }

  // Selection bracket: a fixed 64px corner reticle on the primary selection.
  const bool bracket = editor.has_selection && editor.sel_on_screen;
  SetVisible("ed_select", bracket);
  if (bracket) {
    constexpr float kHalf = 32.0f;
    setLeft("ed_select", editor.sel_screen[0] - kHalf);
    SetStyleField("ed_select", [](ugui::Style& s, float v) { s.top = ugui::Length::Px(v); },
                  editor.sel_screen[1] - kHalf);
  }

  // Status bar.
  setText("ed_status_left", editor.status.empty() ? "Ready" : editor.status);
  setText("ed_grid_t", editor.grid_label);
  SetBackground("ed_snap", editor.snapping ? kEdAccent : kEdToggleOff);
  setLeft("ed_snap_k", editor.snapping ? 18.0f : 2.0f);
  {
    char b[32];
    std::snprintf(b, sizeof(b), "fps %d", last_fps);
    setText("ed_fps", b);
  }
}

bool GameUi::Impl::RouteEditorClick(ugui::wid target) {
  if (!editor_sink || !editor.active) return false;
  // Climb from the clicked widget (the deepest hit) to the nearest editor-handled
  // name. Tree rows distinguish the eye / expand children by name suffix.
  ugui::wid w = target;
  for (int depth = 0; depth < 8 && w.valid(); ++depth) {
    const ugui::WidgetNode* n = ui.world().Get<ugui::WidgetNode>(w);
    if (n) {
      const std::string name = n->name.c_str();
      auto pref = [&](const char* p) -> int {
        const size_t pl = std::strlen(p);
        if (name.size() >= pl && name.compare(0, pl, p) == 0) return std::atoi(name.c_str() + pl);
        return -1;
      };
      auto has = [&](const char* sub) { return name.find(sub) != std::string::npos; };
      using K = EditorUiEvent::Kind;
      EditorUiEvent e;
      if (int i = pref("ed_trow"); i >= 0) {
        e.index = i;
        e.kind = has("_eye") ? K::kTreeEye : has("_exp") ? K::kTreeExpand : K::kTreeSelect;
        editor_sink(e);
        return true;
      }
      if (int i = pref("btn_tool"); i >= 0) { e.kind = K::kTool; e.index = i; editor_sink(e); return true; }
      if (int i = pref("btn_giz"); i >= 0) { e.kind = K::kGizmo; e.index = i; editor_sink(e); return true; }
      if (int i = pref("btn_ltab"); i >= 0) { e.kind = K::kLeftTab; e.index = i; editor_sink(e); return true; }
      if (int i = pref("btn_btab"); i >= 0) { e.kind = K::kCategory; e.index = i; editor_sink(e); return true; }
      if (int i = pref("cl_row"); i >= 0) { e.kind = K::kCategory; e.index = i; editor_sink(e); return true; }
      if (int i = pref("card"); i >= 0) { e.kind = K::kPickCard; e.index = i; editor_sink(e); return true; }
      if (name == "ed_scene_clear") { e.kind = K::kClearScene; editor_sink(e); return true; }
      if (name == "ed_scene_search" || name == "ed_scene_search_text") { e.kind = K::kFocusScene; editor_sink(e); return true; }
      if (name == "ed_asset_clear") { e.kind = K::kClearAsset; editor_sink(e); return true; }
      if (name == "ed_asset_search" || name == "ed_asset_search_text") { e.kind = K::kFocusAsset; editor_sink(e); return true; }
      if (name == "ed_cardprev") { e.kind = K::kCardScroll; e.index = -1; editor_sink(e); return true; }
      if (name == "ed_cardnext") { e.kind = K::kCardScroll; e.index = 1; editor_sink(e); return true; }
      if (name == "btn_treeup") { e.kind = K::kTreeScroll; e.index = -1; editor_sink(e); return true; }
      if (name == "btn_treedn") { e.kind = K::kTreeScroll; e.index = 1; editor_sink(e); return true; }
      if (name == "ed_snap") { e.kind = K::kSnapToggle; editor_sink(e); return true; }
      if (name == "ed_grid") { e.kind = K::kGridCycle; editor_sink(e); return true; }
    }
    const ugui::Hierarchy* h = ui.world().Get<ugui::Hierarchy>(w);
    w = h ? h->parent : ugui::wid{};
  }
  return false;
}

void GameUi::Impl::ApplyMainMenu() {
  SetVisible("mainmenu", main_menu_open);
  if (!main_menu_open) {
    mm_prev_open = false;
    return;
  }
  mm_prev_open = true;
  auto setText = [&](const std::string& n, const std::string& t) {
    ugui::SetText(ui.FindWidget(n.c_str()), t.c_str());
  };

  // Columns: live backdrop image (only when a texture is set), selection accent,
  // label text + availability dimming.
  for (int i = 0; i < kMenuUniverses; ++i) {
    const std::string id = std::to_string(i);
    const bool has_bg = mm_backdrop[i] != 0;
    if (has_bg)
      ugui::SetImageTexture(ui.FindWidget(("mm_bg" + id).c_str()), mm_backdrop[i], 1.0f, 1.0f);
    SetVisible(("mm_bg" + id).c_str(), has_bg);
    SetVisible(("mm_sel" + id).c_str(), i == mm_universe);
    if (i < static_cast<int>(mm_universe_names.size())) setText("mm_labt" + id, mm_universe_names[i]);
    const bool avail = i >= static_cast<int>(mm_available.size()) || mm_available[i];
    SetTextColor(("mm_labt" + id).c_str(), i == mm_universe ? Rgba(0xffffffffu)
                                           : avail            ? Rgba(0xdfe4efffu)
                                                              : Rgba(0x596071ffu));
  }

  // Left nav: caret + highlight on the selected row (QUIT reads red).
  for (int i = 0; i < kMenuNavItems; ++i) {
    const std::string id = std::to_string(i);
    const bool on = i == mm_nav;
    SetVisible(("mm_caret" + id).c_str(), on);
    const bool quit = i == kMenuNavItems - 1;
    SetTextColor(("mm_navt" + id).c_str(), on ? (quit ? Rgba(0xff9a8affu) : Rgba(0xffcc55ffu))
                                              : Rgba(0x9aa3b6ffu));
    SetTextColor(("mm_navs" + id).c_str(), on ? Rgba(0xaeb6c8ffu) : Rgba(0x6b7488ffu));
  }

  // Player banner.
  setText("mm_pname", mm_stats.player_name);
  setText("mm_plevel", "Level " + std::to_string(mm_stats.level));
  setText("mm_pcount", std::to_string(mm_stats.players_online > 0 ? mm_stats.players_online : 3));

  // Sub-screen overlay: title + which body is shown. Body visibility is set
  // unconditionally (not only when the screen is open) so a body never lingers
  // visible while its screen is collapsed.
  SetVisible("mm_screen", mm_screen != 0);
  SetVisible("mm_body_mp", mm_screen == 1);
  SetVisible("mm_body_mods", mm_screen == 2);
  SetVisible("mm_body_settings", mm_screen == 3);
  SetVisible("mm_body_profile", mm_screen == 4);
  if (mm_screen != 0) {
    const char* titles[5] = {"", "MULTIPLAYER", "MODS", "SETTINGS", "PROFILE"};
    setText("mm_screen_title", titles[mm_screen]);
  }
  if (mm_screen == 1) {
    setText("mm_mp_universe", mm_universe < static_cast<int>(mm_universe_names.size())
                                  ? mm_universe_names[mm_universe]
                                  : "");
    setText("mm_mp_status", mm_stats.net_status.empty() ? "Offline" : mm_stats.net_status);
  }
  if (mm_screen == 2) {
    for (int i = 0; i < kMenuModRows; ++i) {
      const std::string id = std::to_string(i);
      const bool row = i < static_cast<int>(mm_mods.size());
      SetVisible(("mm_mod" + id).c_str(), row);
      if (row) setText("mm_modt" + id, mm_mods[i]);
    }
    SetVisible("mm_mods_empty", mm_mods.empty());
  }
  if (mm_screen == 4) {
    setText("mm_pf_name", mm_stats.player_name);
    std::string sub = "Level " + std::to_string(mm_stats.level);
    if (!mm_stats.universe.empty()) sub += "   •   " + mm_stats.universe;
    setText("mm_pf_sub", sub);
    auto bar = [&](const char* fill, float v) {
      SetStyleField(
          fill, [](ugui::Style& s, float x) { s.width = ugui::Length::Pct(x); },
          std::clamp(v, 0.0f, 1.0f) * 100.0f);
    };
    bar("mm_pf_health", mm_stats.health);
    bar("mm_pf_magicka", mm_stats.magicka);
    bar("mm_pf_stamina", mm_stats.stamina);
    setText("mm_pf_gold", mm_stats.in_game ? std::to_string(mm_stats.gold) : "-");
    setText("mm_pf_quests", mm_stats.in_game ? std::to_string(mm_stats.active_quests) : "-");
    setText("mm_pf_loc", mm_stats.location.empty() ? "-" : mm_stats.location);
    SetVisible("mm_pf_hint", !mm_stats.in_game);
  }
}

void GameUi::Impl::ActivateNav() {
  switch (mm_nav) {
    case 0:  // PLAY: enter the selected universe (skip if its data is missing)
      if (mm_universe < static_cast<int>(mm_available.size()) && !mm_available[mm_universe]) return;
      mm_request.kind = MainMenuRequest::Kind::kEnterUniverse;
      mm_request.universe = mm_universe;
      mm_request.multiplayer = false;
      break;
    case 1: mm_screen = 1; break;  // MULTIPLAYER
    case 2: mm_screen = 2; break;  // MODS
    case 3: mm_screen = 3; break;  // SETTINGS
    case 4: mm_screen = 4; break;  // PROFILE
    case 5: mm_request.kind = MainMenuRequest::Kind::kQuit; break;  // QUIT
  }
}

bool GameUi::Impl::RouteMainMenuClick(ugui::wid target) {
  if (!main_menu_open) return false;
  ugui::wid w = target;
  for (int depth = 0; depth < 10 && w.valid(); ++depth) {
    const ugui::WidgetNode* n = ui.world().Get<ugui::WidgetNode>(w);
    if (n) {
      const std::string name = n->name.c_str();
      // Match "<prefix><digit>" so labels like mm_navt2 don't alias mm_nav.
      auto pref = [&](const char* p) -> int {
        const size_t pl = std::strlen(p);
        if (name.size() > pl && name.compare(0, pl, p) == 0 && name[pl] >= '0' && name[pl] <= '9')
          return std::atoi(name.c_str() + pl);
        return -1;
      };
      using K = MainMenuRequest::Kind;
      if (name == "mm_back") { mm_screen = 0; return true; }
      if (name == "mm_mp_host") {
        mm_mp_mode = 0;
        mm_request.kind = K::kHostServer;
        mm_request.universe = mm_universe;
        return true;
      }
      if (name == "mm_mp_join") {
        mm_mp_mode = 1;
        mm_request.kind = K::kJoinServer;
        mm_request.universe = mm_universe;
        return true;
      }
      if (int i = pref("mm_nav"); i >= 0) { mm_nav = i; ActivateNav(); return true; }
      if (int i = pref("mm_col"); i >= 0) {
        if (mm_universe == i && mm_screen == 0) { mm_nav = 0; ActivateNav(); }  // re-click = play
        else mm_universe = i;
        return true;
      }
      if (int i = pref("mm_icon"); i >= 0) {
        if (i == 0) { mm_nav = 1; ActivateNav(); }       // globe -> multiplayer
        else if (i == 3) { mm_nav = 3; ActivateNav(); }  // gear -> settings
        return true;                                     // discord/news consume the click
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
    if (impl->RouteMainMenuClick(w)) return;  // the front menu owns this click
    if (impl->RouteEditorClick(w)) return;    // editor overlay owns this click
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

  // Editor overlay starts collapsed; the engine reveals it on F4.
  impl_->SetVisible("editor_root", false);

  // Debug aid: RECREATION_UI_MENU opens the pause menu at startup.
  if (std::getenv("RECREATION_UI_MENU")) impl_->menu_open = true;
  impl_->ApplyMenuVisibility();  // menu starts hidden unless forced open
  // Debug aid: RECREATION_MAIN_MENU opens the NEXUS front menu at startup.
  if (std::getenv("RECREATION_MAIN_MENU")) impl_->main_menu_open = true;
  impl_->ApplyMainMenu();
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

void GameUi::OpenMainMenu() {
  if (!impl_->initialized) return;
  impl_->main_menu_open = true;
  impl_->mm_screen = 0;
  impl_->ApplyMainMenu();
}

void GameUi::CloseMainMenu() {
  if (!impl_->initialized) return;
  impl_->main_menu_open = false;
  impl_->ApplyMainMenu();
}

bool GameUi::main_menu_open() const { return impl_->initialized && impl_->main_menu_open; }

void GameUi::MainMenuMove(int dx, int dy) {
  if (!impl_->initialized || !impl_->main_menu_open || impl_->mm_screen != 0) return;
  if (dy) impl_->mm_nav = (impl_->mm_nav + dy + kMenuNavItems) % kMenuNavItems;
  if (dx) impl_->mm_universe = std::clamp(impl_->mm_universe + dx, 0, kMenuUniverses - 1);
}

void GameUi::MainMenuActivate() {
  if (!impl_->initialized || !impl_->main_menu_open || impl_->mm_screen != 0) return;
  impl_->ActivateNav();
}

bool GameUi::MainMenuBack() {
  if (!impl_->initialized || !impl_->main_menu_open) return false;
  if (impl_->mm_screen != 0) {
    impl_->mm_screen = 0;
    return true;
  }
  return false;
}

bool GameUi::MainMenuAtRoot() const {
  return impl_->initialized && impl_->main_menu_open && impl_->mm_screen == 0;
}

void GameUi::SetMainMenuUniverses(const std::vector<std::string>& names,
                                  const std::vector<bool>& available) {
  if (!impl_->initialized) return;
  if (!names.empty()) impl_->mm_universe_names = names;
  if (!available.empty()) impl_->mm_available = available;
}

void GameUi::SetMainMenuBackdrop(int universe, u64 texture) {
  if (!impl_->initialized || universe < 0 || universe >= kMenuUniverses) return;
  impl_->mm_backdrop[universe] = texture;
}

void GameUi::SetMainMenuStats(const MainMenuStats& stats) {
  if (impl_->initialized) impl_->mm_stats = stats;
}

void GameUi::SetMainMenuMods(const std::vector<std::string>& mods) {
  if (impl_->initialized) impl_->mm_mods = mods;
}

MainMenuRequest GameUi::PollMainMenuRequest() {
  MainMenuRequest r;
  if (impl_->initialized) {
    r = impl_->mm_request;
    impl_->mm_request = MainMenuRequest{};  // consume
  }
  return r;
}

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

u64 GameUi::CreateUiTexture(int width, int height, const u8* rgba) {
  if (!impl_->initialized || !rgba || width <= 0 || height <= 0) return 0;
  return impl_->backend.CreateTexture(static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                      ugui::RHIFormat::kRgba8Unorm, rgba, ugui::RHIFilter::kLinear);
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
  impl->last_fps = static_cast<int>(frame_delta > 0 ? 1.0f / frame_delta + 0.5f : 0.0f);
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

  // NEXUS main menu (the startup front screen, on top of everything).
  impl->ApplyMainMenu();

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
u64 GameUi::CreateUiTexture(int, int, const u8*) { return 0; }
void GameUi::ToggleMenu() {}
bool GameUi::menu_open() const { return false; }
bool GameUi::quit_requested() const { return false; }
void GameUi::OpenMainMenu() {}
void GameUi::CloseMainMenu() {}
bool GameUi::main_menu_open() const { return false; }
void GameUi::MainMenuMove(int, int) {}
void GameUi::MainMenuActivate() {}
bool GameUi::MainMenuBack() { return false; }
bool GameUi::MainMenuAtRoot() const { return false; }
void GameUi::SetMainMenuUniverses(const std::vector<std::string>&, const std::vector<bool>&) {}
void GameUi::SetMainMenuBackdrop(int, u64) {}
void GameUi::SetMainMenuStats(const MainMenuStats&) {}
void GameUi::SetMainMenuMods(const std::vector<std::string>&) {}
MainMenuRequest GameUi::PollMainMenuRequest() { return {}; }

}  // namespace rec

#endif  // RECREATION_HAS_UGUI
