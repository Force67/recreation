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
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <base/option.h>

#include "core/log.h"
#include "core/window.h"
#include "gui_backend.h"
#include "render/core/renderer.h"
#include "render/rhi/vulkan_interop.h"
#include "ugui_platform.h"

namespace rec {
namespace {

namespace fs = std::filesystem;

// Config options, populated from the environment once at startup (see
// base::InitOptionsFromEnv). Namespace scope so they register before that runs
// and stay visible to every use below. UiDirOpt avoids colliding with UiDir().
base::Option<const char*> UiDirOpt{"ui.dir", nullptr, "RECREATION_UI_DIR"};
base::Option<const char*> UiFont{"ui.font", nullptr, "RECREATION_UI_FONT"};
base::Option<const char*> UiFontMono{"ui.font.mono", nullptr, "RECREATION_UI_FONT_MONO"};
base::Option<bool> UiHotReload{"ui.hot.reload", false, "RECREATION_UI_HOT_RELOAD"};
base::Option<bool> UiMenu{"ui.menu", false, "RECREATION_UI_MENU"};
base::Option<bool> MainMenu{"main.menu", false, "RECREATION_MAIN_MENU"};
base::Option<bool> FirstRun{"first.run", false, "RECREATION_FIRST_RUN"};
base::Option<const char*> FirstRunStep{"first.run.step", nullptr, "RECREATION_FIRST_RUN_STEP"};

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
constexpr int kWarHoldRows = 9;         // Skyrim's nine holds on the war-map panel
constexpr int kContainerRows = 14;      // item rows in the container loot panel
constexpr int kHudGaugeRows = 6;        // pooled managed-gameplay gauge bars (oxygen, rads, ...)
constexpr int kChatRows = 8;            // visible lines in the multiplayer chat box
constexpr int kScoreRows = 12;          // player rows in the multiplayer scoreboard
constexpr int kPromptRows = 3;          // stacked multiplayer interaction prompts
constexpr int kCompassBlips = 8;        // map-blip pips drawn on the compass
constexpr int kNametags = 16;           // floating world-space player nametags
constexpr float kToastSeconds = 4.0f;

// NEXUS main menu.
constexpr int kMenuUniverses = 3;  // Skyrim, Fallout 4, Starfield
constexpr int kMenuNavItems = 6;   // PLAY MULTIPLAYER MODS SETTINGS PROFILE QUIT
constexpr int kMenuModRows = 16;   // pooled rows on the Mods sub-screen
constexpr int kMenuNewsRows = 3;   // pooled rows on the NEWS rail
constexpr int kFirstRunSteps = 5;  // welcome, locate, preferences, mods, ready
constexpr int kFirstRunGames = 3;  // game rows on the locate page

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

// --- UI markup fragments (runtime/ui/*.ugui) --------------------------------
// The static screens live in editable .ugui files so they can be tweaked and
// hot-reloaded without a rebuild. The procedural screens (the scrolling compass
// topbar, the map editor, the NEXUS main menu) are generated in code and
// concatenated in as siblings of root. See RECREATION_UI_DIR (override the
// fragment directory) and RECREATION_UI_HOT_RELOAD (reload on file change).

// The .ugui fragments composed into root, in draw order. Also the hot-reload
// watch list.
const char* const kUiFragments[] = {
    "hud.ugui",       "vitals.ugui",  "readout.ugui",  "quest.ugui",
    "hud_gauge.ugui", "chat.ugui", "scoreboard.ugui", "mp_prompt.ugui", "nametag.ugui",
    "journal.ugui", "war_map.ugui", "dialogue.ugui", "container.ugui",
    "pause_menu.ugui", "main_menu.ugui", "first_run.ugui",
};

// Directory holding the .ugui fragments: RECREATION_UI_DIR, else the compiled-in
// source path, else a cwd-relative fallback.
fs::path UiDir() {
  if (const char* env = UiDirOpt.get(); env && *env) return env;
#ifdef RECREATION_UI_DIR_DEFAULT
  return fs::path(RECREATION_UI_DIR_DEFAULT);
#else
  return fs::path("runtime/ui");
#endif
}

// Read one .ugui fragment. Returns its text, or "" (with a warning) if missing.
std::string LoadUiFragment(const char* name) {
  const fs::path p = UiDir() / name;
  std::ifstream f(p, std::ios::binary);
  if (!f) {
    REC_WARN("ui: fragment not found: {}", p.string());
    return {};
  }
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// The scrolling compass topbar. Procedural: the cardinal strip is generated per
// index (colour/size by direction), so it stays in code rather than a fragment.
std::string BuildTopbarSection() {
  std::string s = R"(
  panel topbar {
    position: absolute; top: 0; left: 0; width: 100vw; height: 64;
    layout: row; justify: center; align: start; padding: 16 0 0 0;
    panel compass_window {
      width: 340; height: 30; position: relative; overflow: hidden;
      panel compass_strip {
        position: absolute; top: 0; left: 0; height: 30; width: 1680;
        layout: row; align: center;
)";
  const int count = 8 * kCompassTurns;
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
)";
  // A pool of map-blip pips, positioned and coloured each frame from SetCompassBlips.
  for (int i = 0; i < kCompassBlips; ++i) {
    s += "      panel blip_pip" + std::to_string(i) +
         " { position: absolute; top: 11; left: 0; width: 7; height: 7; corner-radius: 4;"
         " background: #ffffff; border-color: #000000aa; border-width: 1; visibility: collapsed; }\n";
  }
  s += R"(
    }
  }
)";
  return s;
}

// Assemble the whole UI tree: the root, the procedural sections generated in
// code (topbar, editor, main menu), and the static screens loaded from the
// .ugui fragments, all siblings of root in draw order. The pause/front menus are
// concatenated last so they overlay everything.
std::string BuildUi() {
  std::string s = "\npanel root {\n  width: 100vw; height: 100vh; position: relative;\n";
  s += BuildTopbarSection();              // procedural: scrolling compass
  s += LoadUiFragment("hud.ugui");        // crosshair
  s += LoadUiFragment("vitals.ugui");
  s += LoadUiFragment("readout.ugui");
  s += LoadUiFragment("quest.ugui");
  s += LoadUiFragment("hud_gauge.ugui");
  s += LoadUiFragment("chat.ugui");
  s += LoadUiFragment("scoreboard.ugui");
  s += LoadUiFragment("mp_prompt.ugui");
  s += LoadUiFragment("nametag.ugui");
  s += LoadUiFragment("journal.ugui");
  s += LoadUiFragment("war_map.ugui");
  s += LoadUiFragment("dialogue.ugui");
  s += LoadUiFragment("container.ugui");
  s += BuildEditorSection();              // procedural: Glyph icons; before the menu
  s += LoadUiFragment("pause_menu.ugui");
  s += LoadUiFragment("main_menu.ugui");
  s += LoadUiFragment("first_run.ugui");  // out-of-box wizard, overlays the menu
  s += "}\n";
  return s;
}

const char* FindFont() {
  static std::string resolved;
  if (const char* env = UiFont.get(); env && fs::exists(env)) {
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

// The monospace face for technical/data text (load-order indices, ids, paths).
// fontconfig first, then the usual DejaVu Sans Mono locations. Null if none.
const char* FindMonoFont() {
  static std::string resolved;
  if (const char* env = UiFontMono.get(); env && fs::exists(env)) {
    resolved = env;
    return resolved.c_str();
  }
#if !defined(_WIN32)
  if (FILE* p = popen("fc-match -f '%{file}' 'monospace:style=Regular' 2>/dev/null", "r")) {
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
      "C:/Windows/Fonts/consola.ttf",
#else
      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
      "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
      "/run/current-system/sw/share/X11/fonts/DejaVuSansMono.ttf",
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
  SettingsRequest settings_request;  // raised by the settings panel, polled by the engine
  bool prev_mouse[3] = {};
  bool prev_pad[static_cast<int>(GamepadButton::kCount)] = {};  // gamepad edge tracking
  float stamina = 1.0f;
  int last_fps = 0;  // last computed fps, shown in the editor status bar

  // Hot reload of the .ugui fragments (RECREATION_UI_HOT_RELOAD). Watches each
  // fragment's mtime and rebuilds the tree when one changes.
  bool hot_reload = false;
  float reload_timer = 0.0f;  // throttle the mtime poll
  std::vector<fs::file_time_type> fragment_mtimes;

  // Quest HUD state, set by the engine and applied each frame.
  HudQuest quest;
  std::vector<HudGauge> hud_gauges;  // managed gameplay bars (oxygen, rads, ...)
  std::vector<std::string> chat_lines;  // multiplayer chat box, newest last
  bool scoreboard_open = false;         // multiplayer scoreboard (hold-Tab list)
  std::string scoreboard_title;
  std::string scoreboard_header;
  std::vector<std::string> scoreboard_rows;
  std::vector<std::string> mp_prompts;  // multiplayer interaction prompts
  std::vector<GameUi::CompassBlip> compass_blips;  // map blips on the compass
  std::vector<GameUi::Nametag> nametags;           // floating world-space labels
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

  // War-map overlay, driven by the managed Civil War campaign.
  bool war_map_open = false;
  std::vector<GameUi::WarHoldEntry> war_holds;
  float war_progress = 0.0f;

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
  std::vector<MenuNewsItem> mm_news;
  u64 mm_backdrop[kMenuUniverses] = {0, 0, 0};
  std::vector<std::pair<std::string, u64>> mm_glyphs;  // emblem widget -> texture
  bool mm_prev_open = false;  // edge-detect to hide the gameplay HUD while open

  // First-run setup wizard state. The wizard owns its page (fr_step) and its
  // interactive selections (dropdowns, toggles); the engine pushes the located
  // games / mods dir into fr_view and consumes the request raised below.
  bool first_run_open = false;
  int fr_step = 0;            // 0 welcome .. 4 ready
  int fr_mode = 0;           // default-mode dropdown selection
  int fr_diff = 1;           // difficulty dropdown selection
  int fr_dropdown = -1;      // open popover: -1 none, 0 mode, 1 difficulty
  bool fr_check[3] = {true, true, true};  // enable mods / diagnostics / updates
  FirstRunView fr_view;
  FirstRunRequest fr_request;

  // Drives every main-menu widget from the state above each frame; collapses the
  // whole overlay when closed. Activate runs the highlighted nav item.
  void ApplyMainMenu();
  void ActivateNav();
  // Climbs from a clicked widget to the nearest menu-handled name and acts on it.
  // Returns true if it consumed the click.
  bool RouteMainMenuClick(ugui::wid target);

  // First-run wizard: drive every widget from the state above; route a click to
  // the page it belongs to; advance/retreat the page (the keyboard helpers and
  // the primary button share AdvanceFirstRun). fr_located() counts found games.
  void ApplyFirstRun();
  bool RouteFirstRunClick(ugui::wid target);
  void AdvanceFirstRun();
  void RetreatFirstRun();
  int fr_located() const {
    int n = 0;
    for (const auto& g : fr_view.games)
      if (g.located) ++n;
    return n;
  }

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

  // --- .ugui hot reload -----------------------------------------------------
  // Snapshot each fragment's last-write time so FragmentsChanged() can detect an
  // edit. A missing file records a default time and simply won't trigger.
  void CaptureFragmentMtimes() {
    fragment_mtimes.clear();
    for (const char* name : kUiFragments) {
      std::error_code ec;
      fragment_mtimes.push_back(fs::last_write_time(UiDir() / name, ec));
    }
  }
  bool FragmentsChanged() const {
    const size_t n = sizeof(kUiFragments) / sizeof(*kUiFragments);
    for (size_t i = 0; i < n && i < fragment_mtimes.size(); ++i) {
      std::error_code ec;
      const auto t = fs::last_write_time(UiDir() / kUiFragments[i], ec);
      if (!ec && t != fragment_mtimes[i]) return true;
    }
    return false;
  }
  // Reassemble the tree from the (edited) fragments and reapply the live
  // visibility state the rebuild reset to markup defaults. Per-frame value
  // updates (HUD text, editor view, main-menu data) refresh the rest next frame.
  void ReloadUi() {
    const std::string doc = BuildUi();
    ui.LoadUiString(doc.c_str(), "hud");
    CaptureFragmentMtimes();
    const bool hud = !editor.active;
    SetVisible("topbar", hud);
    SetVisible("crosshair", hud);
    SetVisible("vitals", hud);
    SetVisible("readout", hud);
    SetVisible("editor_root", editor.active);
    editor_prev_active = editor.active;
    ApplyMenuVisibility();
    ApplyMainMenu();
    ApplyFirstRun();
    REC_INFO("ui: hot-reloaded {} .ugui fragment(s)", sizeof(kUiFragments) / sizeof(*kUiFragments));
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

  // Emblems: rebind each frame so they survive a hot-reload tree rebuild.
  for (const auto& [name, tex] : mm_glyphs)
    if (tex) ugui::SetImageTexture(ui.FindWidget(name.c_str()), tex, 1.0f, 1.0f);

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

  // Left nav: caret + highlight on the selected row (QUIT reads red). The row's
  // :selected state carries the eased pill background; the caret and text colour
  // track the same index so keyboard and mouse share one highlight.
  for (int i = 0; i < kMenuNavItems; ++i) {
    const std::string id = std::to_string(i);
    const bool on = i == mm_nav;
    ugui::SetSelected(ui.world(), ui.FindWidget(("mm_nav" + id).c_str()), on);
    SetVisible(("mm_caret" + id).c_str(), on);
    const bool quit = i == kMenuNavItems - 1;
    SetTextColor(("mm_navt" + id).c_str(), on ? (quit ? Rgba(0xff9a8affu) : Rgba(0xffcc55ffu))
                                              : Rgba(0xc2c9d8ffu));
    SetTextColor(("mm_navs" + id).c_str(), on ? Rgba(0xb6bdccffu) : Rgba(0x808a9effu));
  }

  // Profile banner: real handle + system line; peer count only when in session.
  const std::string sysline =
      mm_stats.in_game && !mm_stats.universe.empty()
          ? ("Level " + std::to_string(mm_stats.level) + "  ·  " + mm_stats.universe)
          : (mm_stats.account + (mm_stats.machine.empty() ? "" : "@" + mm_stats.machine));
  setText("mm_pname", mm_stats.player_name.empty() ? mm_stats.account : mm_stats.player_name);
  setText("mm_psys", sysline);
  SetVisible("mm_pnet", mm_stats.players_online > 0);
  setText("mm_pcount", std::to_string(mm_stats.players_online));

  // Build/version stamp.
  setText("mm_build", mm_stats.build.empty() ? "" : ("v" + mm_stats.build));

  // NEWS rail: pooled rows filled from CHANGELOG.md (most-recent first).
  for (int i = 0; i < kMenuNewsRows; ++i) {
    const std::string id = std::to_string(i);
    const bool on = i < static_cast<int>(mm_news.size());
    SetVisible(("news" + id).c_str(), on);
    if (on) {
      setText("news" + id + "_title", mm_news[i].title);
      setText("news" + id + "_sub", mm_news[i].detail);
    }
  }

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
    // Header + real account/system identity (always shown).
    setText("mm_pf_name", mm_stats.player_name.empty() ? mm_stats.account : mm_stats.player_name);
    setText("mm_pf_sub", mm_stats.in_game && !mm_stats.universe.empty()
                             ? ("Playing  ·  " + mm_stats.universe)
                             : "Local profile");
    setText("mm_pf_account", mm_stats.account.empty() ? "-" : mm_stats.account);
    setText("mm_pf_machine", mm_stats.machine.empty() ? "-" : mm_stats.machine);
    const std::string session =
        mm_stats.players_online > 0
            ? (std::to_string(mm_stats.players_online) + " online")
            : (mm_stats.net_status.empty() ? "Single-player" : mm_stats.net_status);
    setText("mm_pf_session", session);
    setText("mm_pf_build", mm_stats.build.empty() ? "-" : ("v" + mm_stats.build));

    // Character vitals/holdings only when a universe is actually loaded.
    SetVisible("mm_pf_char", mm_stats.in_game);
    SetVisible("mm_pf_hint", !mm_stats.in_game);
    if (mm_stats.in_game) {
      auto bar = [&](const char* fill, float v) {
        SetStyleField(
            fill, [](ugui::Style& s, float x) { s.width = ugui::Length::Pct(x); },
            std::clamp(v, 0.0f, 1.0f) * 100.0f);
      };
      bar("mm_pf_health", mm_stats.health);
      bar("mm_pf_magicka", mm_stats.magicka);
      bar("mm_pf_stamina", mm_stats.stamina);
      setText("mm_pf_gold", std::to_string(mm_stats.gold));
      setText("mm_pf_quests", std::to_string(mm_stats.active_quests));
      setText("mm_pf_loc", mm_stats.location.empty() ? "-" : mm_stats.location);
    }
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
      if (name == "act_gear") { mm_screen = 3; return true; }          // settings
      if (name == "act_globe") { mm_request.kind = K::kOpenUrl; mm_request.url = "https://github.com/"; return true; }
      if (name == "act_discord") { mm_request.kind = K::kOpenUrl; mm_request.url = "https://discord.com/"; return true; }
      if (name == "act_changelog" || name == "act_news") { mm_request.kind = K::kOpenUrl; mm_request.url = "https://github.com/"; return true; }
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
    }
    const ugui::Hierarchy* h = ui.world().Get<ugui::Hierarchy>(w);
    w = h ? h->parent : ugui::wid{};
  }
  return false;
}

// First-run setup wizard, a parallel overlay to the main menu. It owns its page
// and selections, the engine feeds it the located games and mods dir, and it
// raises a request the engine consumes (browse, launch, cancel).
void GameUi::Impl::ApplyFirstRun() {
  SetVisible("firstrun", first_run_open);
  if (!first_run_open) return;
  auto setText = [&](const std::string& n, const std::string& t) {
    ugui::SetText(ui.FindWidget(n.c_str()), t.c_str());
  };

  // Pages: exactly one visible.
  for (int i = 0; i < kFirstRunSteps; ++i)
    SetVisible(("fr_step" + std::to_string(i)).c_str(), i == fr_step);

  // Progress rail: nodes filled up to (and including) the current page; the
  // segments between filled nodes glow gold.
  for (int i = 0; i < kFirstRunSteps; ++i) {
    const bool done = i < fr_step, active = i == fr_step;
    SetBackground(("fr_node" + std::to_string(i)).c_str(),
                  active ? Rgba(0xffcc55ffu) : (done ? Rgba(0xffcc55ccu) : Rgba(0x00000000u)));
  }
  for (int i = 0; i < kFirstRunSteps - 1; ++i)
    SetBackground(("fr_seg" + std::to_string(i)).c_str(),
                  i < fr_step ? Rgba(0xffcc55ffu) : Rgba(0xffffff14u));

  // Page 2: located games.
  for (int i = 0; i < kFirstRunGames; ++i) {
    const std::string id = std::to_string(i);
    const bool found = i < static_cast<int>(fr_view.games.size()) && fr_view.games[i].located;
    if (i < static_cast<int>(fr_view.games.size()) && !fr_view.games[i].name.empty())
      setText("fr_name" + id, fr_view.games[i].name);
    setText("fr_path" + id, found ? fr_view.games[i].path : std::string("Not located"));
    SetTextColor(("fr_path" + id).c_str(), found ? Rgba(0x8a93a8ffu) : Rgba(0x6b7488ffu));
    setText("fr_stat" + id, found ? "Located" : "Not found");
    SetTextColor(("fr_stat" + id).c_str(), found ? Rgba(0x5fcf80ffu) : Rgba(0x6b7488ffu));
  }
  // NEXT is gated until at least one game is located (gold when ready).
  SetTextColor("fr_next1_t", fr_located() > 0 ? Rgba(0xffe6a0ffu) : Rgba(0x6b6149ffu));

  // Page 3: dropdowns + toggles.
  static const char* const kModes[4] = {"Exploration", "Story", "Survival", "Sandbox"};
  static const char* const kDiffs[4] = {"Novice", "Normal", "Hard", "Legendary"};
  setText("fr_modeval", kModes[std::clamp(fr_mode, 0, 3)]);
  setText("fr_diffval", kDiffs[std::clamp(fr_diff, 0, 3)]);
  SetVisible("fr_modemenu", fr_dropdown == 0);
  SetVisible("fr_diffmenu", fr_dropdown == 1);
  for (int k = 0; k < 4; ++k) {
    SetVisible(("fr_modetick" + std::to_string(k)).c_str(), k == fr_mode);
    SetVisible(("fr_difftick" + std::to_string(k)).c_str(), k == fr_diff);
  }
  for (int i = 0; i < 3; ++i) {
    SetBackground(("fr_chkbox" + std::to_string(i)).c_str(),
                  fr_check[i] ? Rgba(0xffcc55ffu) : Rgba(0x070a10ffu));
    SetVisible(("fr_chkmk" + std::to_string(i)).c_str(), fr_check[i]);
  }

  // Page 4: mods dir + recommended space.
  setText("fr_modspath_t", fr_view.mods_dir.empty() ? std::string("~/.recreation/mods")
                                                     : fr_view.mods_dir);
  if (!fr_view.space_label.empty()) setText("fr_space", fr_view.space_label);

  // Page 5: a check badge on each located universe.
  for (int i = 0; i < kFirstRunGames; ++i)
    SetVisible(("fr_sealbadge" + std::to_string(i)).c_str(),
               i < static_cast<int>(fr_view.games.size()) && fr_view.games[i].located);
}

void GameUi::Impl::AdvanceFirstRun() {
  fr_dropdown = -1;
  if (fr_step == 1 && fr_located() == 0) return;  // locate page: need one game
  if (fr_step < kFirstRunSteps - 1) {
    ++fr_step;
    return;
  }
  // Last page: advancing launches.
  fr_request.kind = FirstRunRequest::Kind::kLaunch;
  fr_request.mode = fr_mode;
  fr_request.difficulty = fr_diff;
  fr_request.enable_mods = fr_check[0];
  fr_request.share_diagnostics = fr_check[1];
  fr_request.check_updates = fr_check[2];
}

void GameUi::Impl::RetreatFirstRun() {
  fr_dropdown = -1;
  if (fr_step > 0)
    --fr_step;
  else
    fr_request.kind = FirstRunRequest::Kind::kCancel;
}

bool GameUi::Impl::RouteFirstRunClick(ugui::wid target) {
  if (!first_run_open) return false;
  ugui::wid w = target;
  for (int depth = 0; depth < 10 && w.valid(); ++depth) {
    const ugui::WidgetNode* n = ui.world().Get<ugui::WidgetNode>(w);
    if (n) {
      const std::string name = n->name.c_str();
      auto pref = [&](const char* p) -> int {
        const size_t pl = std::strlen(p);
        if (name.size() > pl && name.compare(0, pl, p) == 0 && name[pl] >= '0' && name[pl] <= '9')
          return std::atoi(name.c_str() + pl);
        return -1;
      };
      using K = FirstRunRequest::Kind;
      if (name == "fr_begin") { AdvanceFirstRun(); return true; }
      if (name == "fr_back1" || name == "fr_back2" || name == "fr_back3" || name == "fr_back4") {
        RetreatFirstRun();
        return true;
      }
      if (name == "fr_next1" || name == "fr_next2" || name == "fr_next3") {
        AdvanceFirstRun();
        return true;
      }
      if (name == "fr_launch") {
        AdvanceFirstRun();  // shares the launch path (already on the last page)
        return true;
      }
      if (name == "fr_browse_mods") { fr_request.kind = K::kBrowseMods; return true; }
      if (int i = pref("fr_browse"); i >= 0) {
        fr_request.kind = K::kBrowseGame;
        fr_request.index = i;
        return true;
      }
      if (name == "fr_modesel") { fr_dropdown = fr_dropdown == 0 ? -1 : 0; return true; }
      if (name == "fr_diffsel") { fr_dropdown = fr_dropdown == 1 ? -1 : 1; return true; }
      if (int k = pref("fr_modeopt"); k >= 0) { fr_mode = k; fr_dropdown = -1; return true; }
      if (int k = pref("fr_diffopt"); k >= 0) { fr_diff = k; fr_dropdown = -1; return true; }
      if (int i = pref("fr_chk"); i >= 0 && i < 3) { fr_check[i] = !fr_check[i]; return true; }
    }
    const ugui::Hierarchy* h = ui.world().Get<ugui::Hierarchy>(w);
    w = h ? h->parent : ugui::wid{};
  }
  // A click anywhere else inside the wizard dismisses an open dropdown. Either
  // way the wizard owns every click while it is up (nothing is behind it).
  fr_dropdown = -1;
  return true;
}

GameUi::GameUi() : impl_(std::make_unique<Impl>()) {}
GameUi::~GameUi() { Shutdown(); }

bool GameUi::Initialize(Window& window, render::Renderer& renderer) {
  render::Device* device = renderer.device();
  if (!device || device->is_stub()) return false;
  // The HUD backend records raw Vulkan; on other backends the HUD is unavailable.
  const render::VulkanHandles vk = render::GetVulkanHandles(*device);
  if (vk.device == VK_NULL_HANDLE) return false;

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
  // A monospace face for the technical layer (load-order indices, ids, paths),
  // selectable in markup as `font: mono`. Optional; absent leaves those on sans.
  if (const char* mono_path = FindMonoFont()) {
    ugui::FontHandle mono = impl_->ui.LoadFont(mono_path);
    if (mono != ugui::kInvalidFont) {
      impl_->ui.builder().RegisterFont("mono", mono);
      REC_INFO("ultragui mono font: {}", mono_path);
    }
  }

  ui::GuiRenderBackend::InitInfo bi;
  bi.instance = vk.instance;
  bi.physical_device = vk.physical_device;
  bi.device = vk.device;
  bi.queue_family = vk.graphics_family;
  bi.queue = vk.graphics_queue;
  bi.color_format = render::GetVkFormat(renderer.swapchain_format());
  bi.frames_in_flight = 2;
  if (!impl_->backend.Init(bi)) {
    REC_WARN("ultragui vulkan backend init failed");
    impl_->ui.Shutdown();
    return false;
  }
  impl_->ui.set_texture_backend(&impl_->backend);

  std::string doc = BuildUi();
  impl_->ui.LoadUiString(doc.c_str(), "hud");
  // Hot reload: when enabled, the .ugui fragments are polled for edits and the
  // tree is rebuilt in place (see GameUi::Build). Off by default.
  impl_->hot_reload = bool(UiHotReload);
  impl_->CaptureFragmentMtimes();
  if (impl_->hot_reload) REC_INFO("ui: hot reload on, watching {}", UiDir().string());

  Impl* impl = impl_.get();
  impl_->ui.input().set_on_click([impl](ugui::wid w, ugui::MouseButton btn) {
    if (btn != ugui::MouseButton::kLeft) return;
    if (impl->RouteFirstRunClick(w)) return;  // the setup wizard owns this click
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
    } else if (n->name.rfind("rebind_", 0) == 0 && n->name.find('_', 7) == std::string::npos) {
      // A rebind row (rebind_<N>): ask the engine to capture the next input.
      impl->settings_request = {SettingsRequest::Kind::kRebind, std::atoi(n->name.c_str() + 7), 0};
    } else if (n->name == "btn_skbm_dec") {
      impl->settings_request = {SettingsRequest::Kind::kSensKbm, 0, -1};
    } else if (n->name == "btn_skbm_inc") {
      impl->settings_request = {SettingsRequest::Kind::kSensKbm, 0, +1};
    } else if (n->name == "btn_spad_dec") {
      impl->settings_request = {SettingsRequest::Kind::kSensPad, 0, -1};
    } else if (n->name == "btn_spad_inc") {
      impl->settings_request = {SettingsRequest::Kind::kSensPad, 0, +1};
    } else if (n->name == "btn_invert") {
      impl->settings_request = {SettingsRequest::Kind::kInvertToggle, 0, 0};
    } else if (n->name == "btn_reset") {
      impl->settings_request = {SettingsRequest::Kind::kReset, 0, 0};
    } else if (n->name == "btn_rumble") {
      impl->settings_request = {SettingsRequest::Kind::kTestRumble, 0, 0};
    }
  });

  // Editor overlay starts collapsed; the engine reveals it on F4.
  impl_->SetVisible("editor_root", false);

  // Debug aid: RECREATION_UI_MENU opens the pause menu at startup.
  if (UiMenu) impl_->menu_open = true;
  impl_->ApplyMenuVisibility();  // menu starts hidden unless forced open
  // Debug aid: RECREATION_MAIN_MENU opens the NEXUS front menu at startup.
  if (MainMenu) impl_->main_menu_open = true;
  impl_->ApplyMainMenu();
  // Debug aid: RECREATION_FIRST_RUN opens the setup wizard at startup.
  if (FirstRun) impl_->first_run_open = true;
  impl_->ApplyFirstRun();
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

void GameUi::SetMainMenuGlyph(const std::string& widget, u64 texture) {
  if (!impl_->initialized) return;
  for (auto& [name, tex] : impl_->mm_glyphs)
    if (name == widget) {
      tex = texture;
      return;
    }
  impl_->mm_glyphs.emplace_back(widget, texture);
}

void GameUi::SetMainMenuStats(const MainMenuStats& stats) {
  if (impl_->initialized) impl_->mm_stats = stats;
}

void GameUi::SetMainMenuMods(const std::vector<std::string>& mods) {
  if (impl_->initialized) impl_->mm_mods = mods;
}

void GameUi::SetMainMenuNews(const std::vector<MenuNewsItem>& news) {
  if (impl_->initialized) impl_->mm_news = news;
}

int GameUi::selected_universe() const { return impl_->initialized ? impl_->mm_universe : 0; }

MainMenuRequest GameUi::PollMainMenuRequest() {
  MainMenuRequest r;
  if (impl_->initialized) {
    r = impl_->mm_request;
    impl_->mm_request = MainMenuRequest{};  // consume
  }
  return r;
}

void GameUi::OpenFirstRun() {
  if (!impl_->initialized) return;
  impl_->first_run_open = true;
  impl_->fr_step = 0;
  impl_->fr_dropdown = -1;
  // Debug aid: RECREATION_FIRST_RUN_STEP=<0..4> opens the wizard on that page
  // (so a headless capture can grab any page, not just the welcome screen).
  if (const char* s = FirstRunStep.get()) {
    const int v = std::atoi(s);
    if (v >= 0 && v < kFirstRunSteps) impl_->fr_step = v;
  }
  impl_->ApplyFirstRun();
}

void GameUi::CloseFirstRun() {
  if (!impl_->initialized) return;
  impl_->first_run_open = false;
  impl_->ApplyFirstRun();
}

bool GameUi::first_run_open() const { return impl_->initialized && impl_->first_run_open; }

void GameUi::FirstRunNext() {
  if (impl_->initialized && impl_->first_run_open) impl_->AdvanceFirstRun();
}

void GameUi::FirstRunBack() {
  if (impl_->initialized && impl_->first_run_open) impl_->RetreatFirstRun();
}

void GameUi::SetFirstRunView(const FirstRunView& view) {
  if (impl_->initialized) impl_->fr_view = view;
}

FirstRunRequest GameUi::PollFirstRunRequest() {
  FirstRunRequest r;
  if (impl_->initialized) {
    r = impl_->fr_request;
    impl_->fr_request = FirstRunRequest{};  // consume
  }
  return r;
}

bool GameUi::settings_open() const { return impl_->initialized && impl_->settings_open; }

void GameUi::SetControlsView(const ControlsView& view) {
  if (!impl_->initialized) return;
  Impl* impl = impl_.get();
  for (size_t i = 0; i < view.rows.size(); ++i) {
    const std::string base = "rebind_" + std::to_string(i);
    ugui::SetText(impl->ui.FindWidget((base + "_lbl").c_str()), view.rows[i].label.c_str());
    ugui::SetText(impl->ui.FindWidget((base + "_key").c_str()), view.rows[i].binding.c_str());
  }
  ugui::SetText(impl->ui.FindWidget("sens_kbm_val"), view.sens_kbm.c_str());
  ugui::SetText(impl->ui.FindWidget("sens_pad_val"), view.sens_pad.c_str());
  ugui::SetText(impl->ui.FindWidget("btn_invert"),
                view.invert_y ? "Invert Y: On" : "Invert Y: Off");
  impl->SetVisible("btn_rumble", view.gamepad);  // rumble test only with a pad
}

SettingsRequest GameUi::PollSettingsRequest() {
  SettingsRequest r;
  if (impl_->initialized) {
    r = impl_->settings_request;
    impl_->settings_request = SettingsRequest{};  // consume
  }
  return r;
}

void GameUi::SetQuest(const HudQuest& quest) {
  if (impl_->initialized) impl_->quest = quest;
}

void GameUi::SetChatLines(const std::vector<std::string>& lines) {
  if (impl_->initialized) impl_->chat_lines = lines;
}

void GameUi::SetScoreboard(bool open, const std::string& title, const std::string& header,
                           const std::vector<std::string>& rows) {
  if (!impl_->initialized) return;
  impl_->scoreboard_open = open;
  impl_->scoreboard_title = title;
  impl_->scoreboard_header = header;
  impl_->scoreboard_rows = rows;
}

void GameUi::SetPrompts(const std::vector<std::string>& prompts) {
  if (impl_->initialized) impl_->mp_prompts = prompts;
}

void GameUi::SetCompassBlips(const std::vector<CompassBlip>& blips) {
  if (impl_->initialized) impl_->compass_blips = blips;
}

void GameUi::SetNametags(const std::vector<Nametag>& nametags) {
  if (impl_->initialized) impl_->nametags = nametags;
}

void GameUi::SetHudGauges(const std::vector<HudGauge>& gauges) {
  if (impl_->initialized) impl_->hud_gauges = gauges;
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

void GameUi::SetWarMap(bool open, const std::vector<WarHoldEntry>& holds,
                       float imperial_fraction) {
  if (!impl_->initialized) return;
  impl_->war_map_open = open;
  impl_->war_holds = holds;
  impl_->war_progress = imperial_fraction;
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

void GameUi::Build(Window& window, render::Renderer& renderer, FlyCamera& camera, f32 frame_delta,
                   render::FrameView* view) {
  if (!impl_->initialized) return;
  Impl* impl = impl_.get();

  // Hot reload: poll the .ugui fragments a few times a second and rebuild the
  // tree in place when one is edited. Gated on RECREATION_UI_HOT_RELOAD.
  if (impl->hot_reload) {
    impl->reload_timer += frame_delta;
    if (impl->reload_timer >= 0.25f) {
      impl->reload_timer = 0.0f;
      if (impl->FragmentsChanged()) impl->ReloadUi();
    }
  }

  // Size the UI canvas to the actual backbuffer (swapchain) extent, not the
  // window size — they differ on HiDPI / when the swapchain is clamped, which
  // otherwise lays the menu out over only part of the screen (a black bar).
  const float fb_w = static_cast<float>(renderer.output_width());
  const float fb_h = static_cast<float>(renderer.output_height());
  impl->host.window_width = fb_w > 0.f ? fb_w : static_cast<float>(window.width());
  impl->host.window_height = fb_h > 0.f ? fb_h : static_cast<float>(window.height());

  // Feed the per-frame input snapshot into ultragui's queue, scaling the cursor
  // from window space into the (possibly larger) backbuffer canvas so clicks
  // line up with the widgets.
  const InputState& in = window.input();
  ugui::InputQueue& q = impl->ui.platform()->input_queue();
  const float msx = window.width() > 0 ? fb_w / static_cast<float>(window.width()) : 1.f;
  const float msy = window.height() > 0 ? fb_h / static_cast<float>(window.height()) : 1.f;
  q.PushMove({in.mouse_x * msx, in.mouse_y * msy});
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

  // Feed gamepad + keyboard navigation into ugui's focus ring so menus with
  // tab-index'd widgets (pause / settings) are navigable by pad and keyboard.
  // ugui drives nav/activation internally from these queued events.
  const GamepadState& pad = window.gamepad();
  if (pad.connected) {
    // Map our buttons to ugui's (the enums differ in order); skip unmapped ones.
    static constexpr int kNoUgui = -1;
    auto to_ugui = [](GamepadButton b) -> int {
      switch (b) {
        case GamepadButton::kSouth: return static_cast<int>(ugui::GamepadButton::kA);
        case GamepadButton::kEast: return static_cast<int>(ugui::GamepadButton::kB);
        case GamepadButton::kWest: return static_cast<int>(ugui::GamepadButton::kX);
        case GamepadButton::kNorth: return static_cast<int>(ugui::GamepadButton::kY);
        case GamepadButton::kBack: return static_cast<int>(ugui::GamepadButton::kBack);
        case GamepadButton::kGuide: return static_cast<int>(ugui::GamepadButton::kGuide);
        case GamepadButton::kStart: return static_cast<int>(ugui::GamepadButton::kStart);
        case GamepadButton::kLeftStick: return static_cast<int>(ugui::GamepadButton::kLeftThumb);
        case GamepadButton::kRightStick: return static_cast<int>(ugui::GamepadButton::kRightThumb);
        case GamepadButton::kLeftShoulder: return static_cast<int>(ugui::GamepadButton::kLeftBumper);
        case GamepadButton::kRightShoulder:
          return static_cast<int>(ugui::GamepadButton::kRightBumper);
        case GamepadButton::kDpadUp: return static_cast<int>(ugui::GamepadButton::kDPadUp);
        case GamepadButton::kDpadDown: return static_cast<int>(ugui::GamepadButton::kDPadDown);
        case GamepadButton::kDpadLeft: return static_cast<int>(ugui::GamepadButton::kDPadLeft);
        case GamepadButton::kDpadRight: return static_cast<int>(ugui::GamepadButton::kDPadRight);
        default: return kNoUgui;
      }
    };
    for (int b = 0; b < static_cast<int>(GamepadButton::kCount); ++b) {
      bool down = pad.buttons[b];
      if (down == impl->prev_pad[b]) continue;
      impl->prev_pad[b] = down;
      int u = to_ugui(static_cast<GamepadButton>(b));
      if (u != kNoUgui) q.PushGamepadButton(static_cast<ugui::GamepadButton>(u), down);
    }
    // The stick axes drive repeat navigation; our GamepadAxis order matches ugui's.
    q.PushGamepadAxis(ugui::GamepadAxis::kLeftX, pad.axis(GamepadAxis::kLeftX));
    q.PushGamepadAxis(ugui::GamepadAxis::kLeftY, pad.axis(GamepadAxis::kLeftY));
  }
  // Keyboard focus nav: Tab cycles, Enter/Space activate (ugui uses GLFW codes).
  const int shift_mod = in.key(Key::kLeftShift) ? 0x0001 : 0;
  if (in.key_pressed(Key::kTab)) q.PushKey(258, 0, true, false, shift_mod);
  if (in.key_pressed(Key::kReturn)) q.PushKey(257, 0, true, false, 0);

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

  // Map blips on the compass: one pip per blip at its bearing, in its colour. The
  // pool caps how many show at once; the caller already dropped off-screen ones.
  for (int i = 0; i < kCompassBlips; ++i) {
    const std::string pip = "blip_pip" + std::to_string(i);
    if (static_cast<size_t>(i) < impl->compass_blips.size()) {
      const GameUi::CompassBlip& b = impl->compass_blips[i];
      float off = b.bearing_deg / 45.0f * kCompassLabel;
      off = std::clamp(off, -(kCompassCenter - 8.0f), kCompassCenter - 8.0f);
      impl->SetStyleField(
          pip.c_str(), [](ugui::Style& s, float v) { s.left_offset = ugui::Length::Px(v); },
          kCompassCenter + off - 3.5f);
      impl->SetBackground(pip.c_str(), Rgba(b.color ? b.color : 0xffffffffu));
      impl->SetVisible(pip.c_str(), true);
    } else {
      impl->SetVisible(pip.c_str(), false);
    }
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

  // Managed gameplay gauges (oxygen, radiation, ...): the pooled labeled bar
  // stack above the vitals, one row per active gauge.
  for (int i = 0; i < kHudGaugeRows; ++i) {
    const std::string row = "hud_gauge" + std::to_string(i);
    if (i < static_cast<int>(impl->hud_gauges.size())) {
      const HudGauge& g = impl->hud_gauges[i];
      impl->SetVisible(row.c_str(), true);
      ugui::SetText(impl->ui.FindWidget((row + "_lbl").c_str()), g.label.c_str());
      impl->SetStyleField(
          (row + "_fill").c_str(), [](ugui::Style& s, float v) { s.width = ugui::Length::Pct(v); },
          std::clamp(g.fraction, 0.0f, 1.0f) * 100.0f);
      impl->SetBackground((row + "_fill").c_str(), Rgba(g.color ? g.color : 0x5d92e8ffu));
    } else {
      impl->SetVisible(row.c_str(), false);
    }
  }

  // Multiplayer chat box: the last kChatRows lines, newest at the bottom. The
  // whole box collapses when there is nothing to show.
  const bool chat_on = !impl->chat_lines.empty();
  impl->SetVisible("chat_box", chat_on);
  if (chat_on) {
    const int count = static_cast<int>(impl->chat_lines.size());
    const int first = std::max(0, count - kChatRows);  // tail window
    for (int i = 0; i < kChatRows; ++i) {
      const std::string row = "chat_line" + std::to_string(i);
      const int src = first + i;
      if (src < count) {
        ugui::SetText(impl->ui.FindWidget(row.c_str()), impl->chat_lines[src].c_str());
        impl->SetVisible(row.c_str(), true);
      } else {
        impl->SetVisible(row.c_str(), false);
      }
    }
  }

  // Multiplayer scoreboard: a centered panel of player rows, shown while open.
  impl->SetVisible("scoreboard_box", impl->scoreboard_open);
  if (impl->scoreboard_open) {
    ugui::SetText(impl->ui.FindWidget("scoreboard_title"),
                  impl->scoreboard_title.empty() ? "Players" : impl->scoreboard_title.c_str());
    ugui::SetText(impl->ui.FindWidget("scoreboard_header"), impl->scoreboard_header.c_str());
    for (int i = 0; i < kScoreRows; ++i) {
      const std::string row = "scoreboard_row" + std::to_string(i);
      if (static_cast<size_t>(i) < impl->scoreboard_rows.size()) {
        ugui::SetText(impl->ui.FindWidget(row.c_str()), impl->scoreboard_rows[i].c_str());
        impl->SetVisible(row.c_str(), true);
      } else {
        impl->SetVisible(row.c_str(), false);
      }
    }
  }

  // Multiplayer interaction prompts: a small bottom-centre stack, shown while any
  // are active.
  const bool prompts_on = !impl->mp_prompts.empty();
  impl->SetVisible("mp_prompt_box", prompts_on);
  if (prompts_on) {
    for (int i = 0; i < kPromptRows; ++i) {
      const std::string row = "mp_prompt" + std::to_string(i);
      if (static_cast<size_t>(i) < impl->mp_prompts.size()) {
        ugui::SetText(impl->ui.FindWidget(row.c_str()), impl->mp_prompts[i].c_str());
        impl->SetVisible(row.c_str(), true);
      } else {
        impl->SetVisible(row.c_str(), false);
      }
    }
  }

  // Floating world-space nametags: place each label at its screen position. The
  // engine projects the world position; we centre the pill on it (a rough half
  // width per character) and bias it up so it floats above the player.
  for (int i = 0; i < kNametags; ++i) {
    const std::string tag = "nametag" + std::to_string(i);
    if (static_cast<size_t>(i) < impl->nametags.size()) {
      const GameUi::Nametag& n = impl->nametags[i];
      ugui::SetText(impl->ui.FindWidget((tag + "_txt").c_str()), n.label.c_str());
      const float half_w = 7.0f + n.label.size() * 3.7f;  // approx half the pill width
      impl->SetStyleField(
          tag.c_str(), [](ugui::Style& s, float v) { s.left_offset = ugui::Length::Px(v); },
          n.sx - half_w);
      impl->SetStyleField(
          tag.c_str(), [](ugui::Style& s, float v) { s.top = ugui::Length::Px(v); }, n.sy);
      impl->SetVisible(tag.c_str(), true);
    } else {
      impl->SetVisible(tag.c_str(), false);
    }
  }

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

  // War-map overlay: the Civil War campaign board, each hold and its owner,
  // coloured by side, with the overall war-progress bar.
  impl->SetVisible("war_map_box", impl->war_map_open);
  if (impl->war_map_open) {
    int imperial = 0, stormcloak = 0;
    for (int i = 0; i < kWarHoldRows; ++i) {
      const std::string row = "war_hold" + std::to_string(i);
      if (i < static_cast<int>(impl->war_holds.size())) {
        const GameUi::WarHoldEntry& h = impl->war_holds[i];
        const char* owner = h.owner == 1 ? "Imperial" : h.owner == 2 ? "Stormcloak" : "Contested";
        const ugui::Color col = h.owner == 1   ? Rgba(0x6f9fe8ffu)
                                : h.owner == 2 ? Rgba(0xe86f6fffu)
                                               : Rgba(0xb6bdccffu);
        ugui::SetText(impl->ui.FindWidget(row.c_str()), (h.name + "  --  " + owner).c_str());
        impl->SetTextColor(row.c_str(), col);
        impl->SetVisible(row.c_str(), true);
        if (h.owner == 1) ++imperial;
        if (h.owner == 2) ++stormcloak;
      } else {
        impl->SetVisible(row.c_str(), false);
      }
    }
    char sub[96];
    std::snprintf(sub, sizeof(sub), "Imperial Legion %d   |   Stormcloaks %d", imperial, stormcloak);
    ugui::SetText(impl->ui.FindWidget("war_map_sub"), sub);
    impl->SetStyleField(
        "war_bar_fill", [](ugui::Style& s, float v) { s.width = ugui::Length::Pct(v); },
        std::clamp(impl->war_progress, 0.0f, 1.0f) * 100.0f);
    char prog[96];
    std::snprintf(prog, sizeof(prog), "Imperial control: %d%%",
                  static_cast<int>(std::clamp(impl->war_progress, 0.0f, 1.0f) * 100.0f + 0.5f));
    ugui::SetText(impl->ui.FindWidget("war_map_progress"), prog);
  }

  // Map editor overlay (asset browser, toolbar, inspector, status, reticle).
  impl->ApplyEditorView();

  // NEXUS main menu (the startup front screen, on top of everything).
  impl->ApplyMainMenu();

  // First-run setup wizard (the out-of-box experience, above even the menu).
  impl->ApplyFirstRun();

  // Produce the draw list (input routing + layout + paint, no GPU work).
  const ugui::DrawData& dd = impl->ui.RenderDrawData();
  impl->draw_data = &dd;

  // Tell the renderer whether any widget wants backdrop blur this frame, so it
  // only captures + blurs the backbuffer when a frosted panel is actually shown.
  view->needs_blur = false;
  for (u32 i = 0; i < dd.command_count; ++i) {
    if (dd.commands[i].blur > 0.0f) {
      view->needs_blur = true;
      break;
    }
  }

  // Upload the glyph atlas if it grew this frame.
  if (impl->ui.text_engine().atlas_revision() != impl->font_revision) {
    ugui::Vec2 as = impl->ui.text_engine().atlas_size();
    impl->backend.UpdateFontAtlas(impl->ui.text_engine().atlas_pixels(), static_cast<u32>(as.x),
                                  static_cast<u32>(as.y));
    impl->font_revision = impl->ui.text_engine().atlas_revision();
  }

  impl->backend.NewFrame();
  view->hud_draw = [impl, view](render::CommandList& cmd) {
    // The renderer fills view->blur_source just before this runs (inside the ui
    // pass) with the blurred backdrop for frosted panels; null disables frost.
    impl->backend.SetBackdrop(render::GetVkImageView(view->blur_source),
                              render::GetVkSampler(view->blur_sampler));
    if (impl->draw_data)
      impl->backend.Render(*impl->draw_data, render::GetVkCommandBuffer(cmd));
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
void GameUi::SetChatLines(const std::vector<std::string>&) {}
void GameUi::SetScoreboard(bool, const std::string&, const std::string&,
                           const std::vector<std::string>&) {}
void GameUi::SetPrompts(const std::vector<std::string>&) {}
void GameUi::SetCompassBlips(const std::vector<CompassBlip>&) {}
void GameUi::SetNametags(const std::vector<Nametag>&) {}
void GameUi::SetHudGauges(const std::vector<HudGauge>&) {}
void GameUi::FlashQuestUpdate(const std::string&) {}
void GameUi::SetActivatePrompt(const std::string&) {}
void GameUi::SetHudVisible(bool) {}
void GameUi::SetObjectiveMarker(bool, float, float) {}
void GameUi::SetDialogue(const DialogueView&) {}
void GameUi::SetContainer(const ContainerView&) {}
void GameUi::SetJournal(bool, const std::vector<HudQuest>&, int) {}
void GameUi::SetWarMap(bool, const std::vector<WarHoldEntry>&, float) {}
void GameUi::SetEditorView(const EditorView&) {}
void GameUi::SetEditorEventSink(std::function<void(const EditorUiEvent&)>) {}
u64 GameUi::CreateUiTexture(int, int, const u8*) { return 0; }
void GameUi::ToggleMenu() {}
bool GameUi::menu_open() const { return false; }
bool GameUi::settings_open() const { return false; }
void GameUi::SetControlsView(const ControlsView&) {}
SettingsRequest GameUi::PollSettingsRequest() { return {}; }
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
void GameUi::SetMainMenuNews(const std::vector<MenuNewsItem>&) {}
void GameUi::SetMainMenuGlyph(const std::string&, u64) {}
int GameUi::selected_universe() const { return 0; }
MainMenuRequest GameUi::PollMainMenuRequest() { return {}; }
void GameUi::OpenFirstRun() {}
void GameUi::CloseFirstRun() {}
bool GameUi::first_run_open() const { return false; }
void GameUi::FirstRunNext() {}
void GameUi::FirstRunBack() {}
void GameUi::SetFirstRunView(const FirstRunView&) {}
FirstRunRequest GameUi::PollFirstRunRequest() { return {}; }

}  // namespace rec

#endif  // RECREATION_HAS_UGUI
