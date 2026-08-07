// The map editor overlay: its markup, its per-frame view application, and its
// click routing.

#include "runtime/ui/game_ui_internal.h"

#if defined(RECREATION_HAS_UGUI)

namespace rx {

namespace {
base::String Glyph(const base::String& k, const char* col) {
  base::String c;
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
  if (k == "select") {
    R(4, 3, 2, 11, col);
    R(4, 3, 8, 2, col);
    R(8, 8, 5, 2, col);
  } else if (k == "move") {
    R(8, 2, 2, 14, col, 1);
    R(2, 8, 14, 2, col, 1);
  } else if (k == "rotate") {
    O(3, 3, 12, 12, col, 2, 6);
    R(13, 1, 4, 2, col);
  } else if (k == "scale") {
    O(3, 7, 8, 8, col, 1.5f, 2);
    R(11, 3, 4, 4, col, 1);
  } else if (k == "hand") {
    R(6, 7, 6, 8, col, 2);
    R(6, 4, 2, 5, col, 1);
    R(9, 3, 2, 6, col, 1);
    R(12, 5, 2, 4, col, 1);
  } else if (k == "terrain") {
    R(2, 11, 4, 5, col, 1);
    R(7, 7, 4, 9, col, 1);
    R(12, 9, 4, 7, col, 1);
  } else if (k == "paint") {
    R(4, 3, 9, 5, col, 2);
    R(8, 8, 2, 7, col, 1);
  } else if (k == "play") {
    R(5, 4, 3, 10, col);
    R(8, 6, 3, 6, col);
    R(11, 8, 2, 2, col);
  } else if (k == "save") {
    O(3, 3, 12, 12, col, 1.5f, 2);
    R(6, 3, 6, 3, col);
    R(6, 10, 6, 2, col);
  } else if (k == "undo") {
    R(5, 8, 9, 2, col);
    R(5, 5, 2, 3, col);
    R(5, 10, 2, 3, col);
    R(12, 5, 2, 4, col);
  } else if (k == "redo") {
    R(4, 8, 9, 2, col);
    R(11, 5, 2, 3, col);
    R(11, 10, 2, 3, col);
    R(4, 5, 2, 4, col);
  } else if (k == "gear") {
    O(4, 4, 10, 10, col, 2, 5);
    R(8, 1, 2, 3, col);
    R(8, 14, 2, 3, col);
    R(1, 8, 3, 2, col);
    R(14, 8, 3, 2, col);
  } else if (k == "cube") {
    O(4, 5, 10, 9, col, 1.5f, 1);
    R(4, 5, 10, 3, col);
  } else if (k == "folder") {
    R(3, 6, 12, 8, col, 1);
    R(3, 4, 6, 3, col, 1);
  } else if (k == "sphere") {
    O(3, 3, 12, 12, col, 1.5f, 6);
    R(6, 5, 3, 3, "#ffffff66", 2);
  } else if (k == "magnify") {
    O(3, 3, 9, 9, col, 1.5f, 5);
    R(11, 11, 4, 2, col, 1);
  } else if (k == "caret") {
    R(5, 7, 8, 2, col);
    R(6, 9, 6, 2, col);
    R(8, 11, 2, 2, col);
  } else if (k == "triup") {
    R(8, 7, 2, 2, col);
    R(6, 9, 6, 2, col);
    R(5, 11, 8, 2, col);
  } else if (k == "tridown") {
    R(5, 7, 8, 2, col);
    R(6, 9, 6, 2, col);
    R(8, 11, 2, 2, col);
  } else if (k == "lock") {
    R(4, 8, 10, 7, col, 1);
    O(6, 4, 6, 6, col, 1.5f, 3);
  } else if (k == "grid") {
    R(4, 4, 4, 4, col, 1);
    R(10, 4, 4, 4, col, 1);
    R(4, 10, 4, 4, col, 1);
    R(10, 10, 4, 4, col, 1);
  } else if (k == "plus") {
    R(8, 4, 2, 10, col);
    R(4, 8, 10, 2, col);
  } else if (k == "kebab") {
    R(8, 3, 2, 2, col, 1);
    R(8, 8, 2, 2, col, 1);
    R(8, 13, 2, 2, col, 1);
  } else if (k == "eye") {
    O(3, 6, 12, 7, col, 1.5f, 4);
    R(7, 8, 4, 4, col, 2);
  } else if (k == "minwin") {
    R(4, 9, 10, 2, col);
  } else if (k == "maxwin") {
    O(4, 4, 10, 10, col, 1.5f, 1);
  }
  return "panel { position: relative; width: 18; height: 18; " + c + " }\n";
}

// The map editor overlay, styled as a Creation-Kit-style dock layout: a top
// toolbar (logo + labelled tool cluster + window controls), a left scene/assets
// dock with a hierarchy tree, a right inspector, a bottom asset browser between
// the side docks, and a status bar. Everything is pooled (fixed widget counts
// filled and toggled each frame) and starts hidden; the engine collapses
// editor_root until the editor is on. Names are matched by the click router
// (btn_tool*, btn_giz*, btn_ltab*, ed_trow*, btn_btab*, cl_row*, card*, ...).
}  // namespace

base::String BuildEditorSection() {
  const char* AC = "#ffffff";   // accent indigo
  const char* TXP = "#ffffff";  // primary text / icons
  const char* TXS = "#9a9a9a";  // secondary text
  const char* TXM = "#5e5e5e";  // muted text
  base::String s;
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
      background: #ffffff24; border-color: #ffffffcc; border-width: 1;
    }

    panel ed_select {
      position: absolute; left: 0; top: 0; width: 64; height: 64;
      panel { position: absolute; left: 0; top: 0; width: 16; height: 2; background: #ffffff; }
      panel { position: absolute; left: 0; top: 0; width: 2; height: 16; background: #ffffff; }
      panel { position: absolute; left: 48; top: 0; width: 16; height: 2; background: #ffffff; }
      panel { position: absolute; left: 62; top: 0; width: 2; height: 16; background: #ffffff; }
      panel { position: absolute; left: 0; top: 62; width: 16; height: 2; background: #ffffff; }
      panel { position: absolute; left: 0; top: 48; width: 2; height: 16; background: #ffffff; }
      panel { position: absolute; left: 48; top: 62; width: 16; height: 2; background: #ffffff; }
      panel { position: absolute; left: 62; top: 48; width: 2; height: 16; background: #ffffff; }
    }
)";

  // --- viewport overlays: gizmo bar, perspective chip, axis gizmo ---
  std::snprintf(buf, sizeof(buf),
                "\n    panel ed_gizmobar { position: absolute; left: %g; top: %g; layout: row;"
                " align: center; gap: 2; background: #0a0a0ae8; padding: 4;"
                " border-color: #ffffff14; border-width: 1;\n",
                kEdSceneW + 16.0f, kEdToolbarH + 14.0f);
  s += buf;
  const char* giz[4] = {"hand", "move", "rotate", "scale"};
  for (int i = 0; i < 4; ++i) {
    s += "      panel btn_giz" + base::ToString(i) +
         " { padding: 6; background: #ffffff00; cursor: pointer;"
         " :hover { background: #ffffff14; }\n        " +
         Glyph(giz[i], TXP) + "      }\n";
  }
  s += "    }\n";

  std::snprintf(buf, sizeof(buf),
                "\n    panel ed_persp { position: absolute; right: %g; top: %g; layout: row;"
                " align: center; gap: 8; background: #0a0a0ae8; padding: 7 11;"
                " border-color: #ffffff14; border-width: 1;\n"
                "      text { text: \"Perspective\"; font-size: 12; color: %s; }\n      %s    }\n",
                kEdInspectorW + 16.0f, kEdToolbarH + 14.0f, TXS, Glyph("caret", TXS).c_str());
  s += buf;

  std::snprintf(
      buf, sizeof(buf),
      "\n    panel ed_axis { position: absolute; right: %g; top: %g; width: 60; height: 60;\n"
      // Axis triad: mirrors the 3D gizmo's own colours on purpose (see above).
      "      panel { position: absolute; left: 28; top: 8; width: 2; height: 22; background: "
      "#ffffff; }\n"
      "      text { position: absolute; left: 25; top: 0; font-size: 11; color: #ffffff; text: "
      "\"Y\"; }\n"
      "      panel { position: absolute; left: 29; top: 28; width: 22; height: 2; background: "
      "#ff2e17; }\n"
      "      text { position: absolute; left: 50; top: 22; font-size: 11; color: #ff2e17; text: "
      "\"X\"; }\n"
      "      panel { position: absolute; left: 8; top: 28; width: 22; height: 2; background: "
      "#ffffff; }\n"
      "      text { position: absolute; left: 1; top: 33; font-size: 11; color: #ffffff; text: "
      "\"Z\"; }\n"
      "      panel { position: absolute; left: 26; top: 26; width: 6; height: 6; "
      "background: #ffffff; }\n    }\n",
      kEdInspectorW + 26.0f, kEdToolbarH + 50.0f);
  s += buf;

  // --- top toolbar ---
  std::snprintf(buf, sizeof(buf),
                "\n    panel editor_toolbar { position: absolute; top: 0; left: 0; width: 100vw;"
                " height: %g; layout: row; align: center; justify: space-between; padding: 0 14;"
                " background: #101010f8; border-color: #ffffff12; border-width: 1;\n",
                kEdToolbarH);
  s += buf;
  s += R"(      panel { layout: row; align: center; gap: 11;
        panel { width: 30; height: 30; background: #ffffff; layout: column;
          justify: center; align: center; text { text: "R"; font-size: 17; color: #000000; } }
        text { text: "RECREATION"; font-size: 14; color: #ffffff; letter-spacing: 2; }
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
    s += "        panel btn_tool" + base::ToString(i) +
         " { layout: column; align: center; justify: center; gap: 3; padding: 5 8;"
         " cursor: pointer; background: #ffffff00; :hover { background: "
         "#ffffff12; }\n          " +
         Glyph(tkind[i], TXP) + "          text btn_tool" + base::ToString(i) + "_lbl { text: \"" +
         tlabel[i] + "\"; font-size: 10; color: " + TXS + "; }\n"
         "          panel btn_tool" + base::ToString(i) +
         "_ul { width: 100%; height: 2; background: #ffffff00; }\n        }\n";
  }
  s += "      }\n";  // close ed_tb_tools
  // Right cluster: world chip, settings, help, window controls.
  s += "      panel { layout: row; align: center; gap: 9;\n";
  s += "        panel { layout: row; align: center; gap: 7; background: #000000;"
       " padding: 6 10; border-color: #ffffff14; border-width: 1;\n"
       "          text { text: \"Skyrim\"; font-size: 12; color: #ffffff; }\n          " +
       Glyph("caret", TXS) + "        }\n";
  s += "        panel { padding: 6; background: #ffffff00; cursor: pointer;"
       " :hover { background: #ffffff12; }\n          " +
       Glyph("gear", TXS) + "        }\n";
  s +=
      "        button { text: \"?\"; font-size: 15; color: #9a9a9a; padding: 4 9;"
      " background: #ffffff00; cursor: pointer; :hover { background: #ffffff12; } }\n";
  s += "        panel { layout: row; align: center; gap: 2; margin: 0 0 0 4;\n          " +
       Glyph("minwin", TXM) + "          " + Glyph("maxwin", TXM) +
       "          text { text: \"x\"; font-size: 14; color: #9a9a9a; padding: 0 4; }\n        }\n";
  s += "      }\n    }\n";  // close right cluster, toolbar

  // --- left dock: scene tree / assets ---
  std::snprintf(buf, sizeof(buf),
                "\n    panel editor_scene { position: absolute; left: 0; top: %g; width: %g;"
                " bottom: %g; layout: column; align: start; background: #0d0d0df8;"
                " border-color: #ffffff12; border-width: 1;\n",
                kEdToolbarH, kEdSceneW, kEdStatusH);
  s += buf;
  s += R"(      panel { layout: row; align: center; padding: 4 10 0 10; width: 100%;
        panel btn_ltab0 { layout: column; align: center; gap: 6; padding: 9 12; cursor: pointer; background: #ffffff00;
          text btn_ltab0_t { text: "Scene"; font-size: 13; color: #ffffff; }
          panel btn_ltab0_ul { width: 38; height: 2; background: #ffffff; }
        }
        panel btn_ltab1 { layout: column; align: center; gap: 6; padding: 9 12; cursor: pointer; background: #ffffff00;
          text btn_ltab1_t { text: "Assets"; font-size: 13; color: #5e5e5e; }
          panel btn_ltab1_ul { width: 38; height: 2; background: #ffffff00; }
        }
      }
      panel { width: 100%; height: 1; background: #ffffff10; }
      panel { layout: row; align: center; gap: 8; padding: 10 12 6 12; width: 100%;
        panel ed_scene_search { layout: row; align: center; gap: 7; flex-grow: 1; background: #000000; padding: 7 10; border-color: #ffffff14; border-width: 1; cursor: text;
)";
  s += "          " + Glyph("magnify", TXM) +
       "          text ed_scene_search_text { text: \"Search scene...\"; font-size: 12; color: "
       "#5e5e5e; flex-grow: 1; }\n"
       "          button ed_scene_clear { text: \"x\"; font-size: 12; color: #5e5e5e; padding: 0 2;"
       " background: #ffffff00; cursor: pointer; :hover { color: #ffffff; } }\n        }\n";
  s += "        panel ed_scene_filter { padding: 7; background: #000000;"
       " border-color: #ffffff14; border-width: 1; cursor: pointer; :hover { background: "
       "#ffffff12; }\n          " +
       Glyph("caret", TXM) + "        }\n      }\n";
  // Tree rows (pooled).
  s += "      panel ed_tree { layout: column; align: start; gap: 1; padding: 2 6; width: 100%;"
       " flex-grow: 1; overflow: hidden;\n";
  for (int i = 0; i < kEdTreeRows; ++i) {
    const base::String id = base::ToString(i);
    s += "        panel ed_trow" + id +
         " { layout: row; align: center; gap: 5; padding: 4 6; width: 100%;"
         " cursor: pointer; background: #ffffff00; :hover { background: #ffffff10; }\n"
         "          panel ed_trow" +
         id +
         "_pad { width: 2; height: 1; }\n"
         "          button ed_trow" +
         id +
         "_exp { text: \"\"; font-size: 13; color: #9a9a9a;"
         " width: 14; text-align: center; background: #ffffff00; cursor: pointer; }\n"
         "          panel ed_trow" +
         id +
         "_ico { width: 11; height: 11; background: #5e5e5e; }\n"
         "          text ed_trow" +
         id +
         "_name { text: \"\"; font-size: 12; color: #ffffff; flex-grow: 1; }\n"
         "          panel ed_trow" +
         id +
         "_eye { width: 12; height: 12; background: #c8cfdd; cursor: pointer; }\n"
         "        }\n";
  }
  s += "      }\n";  // close ed_tree
  // Footer: add buttons + tree pager.
  s += "      panel { layout: row; align: center; justify: space-between; padding: 6 10; width: "
       "100%;\n"
       "        panel { layout: row; align: center; gap: 6;\n";
  s += "          panel { padding: 6; background: #ffffff0c; cursor: pointer; "
       ":hover { background: #ffffff18; }\n            " +
       Glyph("plus", TXS) + "          }\n";
  s += "          panel { padding: 6; background: #ffffff0c; cursor: pointer; "
       ":hover { background: #ffffff18; }\n            " +
       Glyph("folder", TXS) + "          }\n";
  s += "          panel { padding: 6; background: #ffffff0c; cursor: pointer; "
       ":hover { background: #ffffff18; }\n            " +
       Glyph("grid", TXS) + "          }\n";
  s += "        }\n        panel { layout: row; align: center; gap: 4;\n";
  s += "          panel btn_treeup { padding: 6; background: #ffffff0c; cursor: "
       "pointer; :hover { background: #ffffff18; }\n            " +
       Glyph("triup", TXS) + "          }\n";
  s += "          panel btn_treedn { padding: 6; background: #ffffff0c; cursor: "
       "pointer; :hover { background: #ffffff18; }\n            " +
       Glyph("tridown", TXS) + "          }\n";
  s += "        }\n      }\n    }\n";  // close footer, editor_scene

  // --- right dock: inspector ---
  auto section = [&](const char* title) {
    s += "        panel { layout: row; align: center; gap: 6; width: 100%; margin: 4 0 0 0;\n      "
         "    " +
         Glyph("caret", TXS) + "          text { text: \"" + title +
         "\"; font-size: 12; color: " + TXP + "; letter-spacing: 1; flex-grow: 1; }\n          " +
         Glyph("kebab", TXM) + "        }\n";
  };
  auto chip = [&](const char* letter, const char* lcol, const base::String& valname) {
    s += "          panel { layout: row; align: center; gap: 4; flex-grow: 1; background: #000000;"
         " padding: 5 6; border-color: #ffffff12; border-width: 1;\n"
         "            text { text: \"" +
         base::String(letter) + "\"; font-size: 11; color: " + lcol +
         "; }\n"
         "            text " +
         valname + " { text: \"0\"; font-size: 11; color: #ffffff; flex-grow: 1; }\n          }\n";
  };
  auto xyzrow = [&](const char* label, const base::String& px, const base::String& py,
                    const base::String& pz, bool lock) {
    s += "        panel { layout: row; align: center; gap: 8; width: 100%;\n"
         "          text { text: \"" +
         base::String(label) + "\"; font-size: 12; color: " + TXS +
         "; width: 56; }\n"
         "          panel { layout: row; align: center; gap: 5; flex-grow: 1;\n";
    chip("X", "#ffffff", px);
    chip("Y", "#ffffff", py);
    chip("Z", "#ffffff", pz);
    if (lock)
      s += "          panel { width: 18; height: 18; layout: column; justify: center; align: "
           "center; " +
           Glyph("lock", TXM) + "          }\n";
    s += "          }\n        }\n";
  };
  auto toggle = [&](const char* label, const base::String& name) {
    s += "        panel { layout: row; align: center; justify: space-between; width: 100%;\n"
         "          text { text: \"" +
         base::String(label) +
         "\"; font-size: 12; color: #9a9a9a; }\n"
         "          panel " +
         name + " { width: 34; height: 18; background: " + AC +
         "; position: relative;\n"
         "            panel " +
         name +
         "_k { position: absolute; left: 18; top: 2; width: 14; height: 14; "
         "background: #ffffff; }\n          }\n        }\n";
  };
  std::snprintf(buf, sizeof(buf),
                "\n    panel editor_inspector { position: absolute; right: 0; top: %g; width: %g;"
                " bottom: %g; layout: column; align: start; background: #0d0d0df8;"
                " border-color: #ffffff12; border-width: 1; overflow: hidden;\n",
                kEdToolbarH, kEdInspectorW, kEdStatusH);
  s += buf;
  s += "      panel { layout: row; align: center; justify: space-between; padding: 12 14; width: "
       "100%;\n"
       "        text { text: \"Inspector\"; font-size: 13; color: #ffffff; letter-spacing: 1; }\n  "
       "      " +
       Glyph("kebab", TXM) + "      }\n";
  s += "      panel { width: 100%; height: 1; background: #ffffff10; }\n";
  s += "      panel ed_insp_empty { layout: column; align: center; justify: center; padding: 40 0; "
       "width: 100%;\n"
       "        text { text: \"No object selected\"; font-size: 12; color: #5e5e5e; } }\n";
  s +=
      R"(      panel ed_terrain_body { layout: column; align: start; padding: 14; gap: 14; width: 100%;
        panel { layout: column; align: start; gap: 5; width: 100%; background: #ffffff1c;
          border-color: #ffffff52; border-width: 1; padding: 12;
          text { text: "LAND HEIGHT EDIT"; font-size: 11; color: #ffffff; letter-spacing: 1; }
          text { text: "Non-destructive worldspace delta"; font-size: 12; color: #ffffff; }
          text { text: "Shared borders rebuild as one lattice."; font-size: 10; color: #5e5e5e; }
        }
        text { text: "BRUSH MODE"; font-size: 10; color: #5e5e5e; letter-spacing: 1; }
        panel { layout: row; align: center; gap: 6; width: 100%;
          panel ed_terrain_mode0 { flex-grow: 1; layout: column; align: center; padding: 8 4; background: #ffffff; cursor: pointer;
            text ed_terrain_mode0_t { text: "Raise"; font-size: 11; color: #ffffff; } }
          panel ed_terrain_mode1 { flex-grow: 1; layout: column; align: center; padding: 8 4; background: #000000; cursor: pointer;
            text ed_terrain_mode1_t { text: "Lower"; font-size: 11; color: #9a9a9a; } }
        }
        panel { layout: row; align: center; gap: 6; width: 100%;
          panel ed_terrain_mode2 { flex-grow: 1; layout: column; align: center; padding: 8 4; background: #000000; cursor: pointer;
            text ed_terrain_mode2_t { text: "Smooth"; font-size: 11; color: #9a9a9a; } }
          panel ed_terrain_mode3 { flex-grow: 1; layout: column; align: center; padding: 8 4; background: #000000; cursor: pointer;
            text ed_terrain_mode3_t { text: "Flatten"; font-size: 11; color: #9a9a9a; } }
        }
        panel { width: 100%; height: 1; background: #ffffff10; }
        panel { layout: row; align: center; justify: space-between; width: 100%;
          panel { layout: column; align: start; gap: 2;
            text { text: "Radius"; font-size: 12; color: #ffffff; }
            text { text: "World-space metres"; font-size: 10; color: #5e5e5e; } }
          panel { layout: row; align: center; gap: 5;
            panel ed_terrain_radius_dec { width: 27; height: 27; background: #000000; layout: column; align: center; justify: center; cursor: pointer;
              text { text: "-"; font-size: 15; color: #9a9a9a; } }
            panel { width: 62; padding: 6 9; background: #000000; text ed_terrain_radius { text: "4.0 m"; font-size: 11; color: #ffffff; text-align: center; } }
            panel ed_terrain_radius_inc { width: 27; height: 27; background: #000000; layout: column; align: center; justify: center; cursor: pointer;
              text { text: "+"; font-size: 15; color: #9a9a9a; } }
          }
        }
        panel { layout: row; align: center; justify: space-between; width: 100%;
          panel { layout: column; align: start; gap: 2;
            text { text: "Strength"; font-size: 12; color: #ffffff; }
            text { text: "Per spaced dab"; font-size: 10; color: #5e5e5e; } }
          panel { layout: row; align: center; gap: 5;
            panel ed_terrain_strength_dec { width: 27; height: 27; background: #000000; layout: column; align: center; justify: center; cursor: pointer;
              text { text: "-"; font-size: 15; color: #9a9a9a; } }
            panel { width: 62; padding: 6 9; background: #000000; text ed_terrain_strength { text: "0.25"; font-size: 11; color: #ffffff; text-align: center; } }
            panel ed_terrain_strength_inc { width: 27; height: 27; background: #000000; layout: column; align: center; justify: center; cursor: pointer;
              text { text: "+"; font-size: 15; color: #9a9a9a; } }
          }
        }
        panel { width: 100%; height: 1; background: #ffffff10; }
        panel { layout: row; align: center; justify: space-between; width: 100%;
          text { text: "Diff samples"; font-size: 11; color: #9a9a9a; }
          text ed_terrain_samples { text: "0"; font-size: 12; color: #ffffff; } }
        panel { layout: row; align: center; justify: space-between; width: 100%;
          text { text: "State"; font-size: 11; color: #9a9a9a; }
          panel ed_terrain_dirty_chip { padding: 4 8; background: #46c46324;
            text ed_terrain_dirty { text: "Saved"; font-size: 10; color: #70d88a; } } }
        panel { layout: column; align: start; gap: 4; width: 100%;
          text { text: "DIFF FILE"; font-size: 10; color: #5e5e5e; letter-spacing: 1; }
          panel { width: 100%; background: #000000; padding: 7 8; overflow: hidden;
            text ed_terrain_path { text: "editor_layout.recterrain"; font-size: 10; color: #9a9a9a; } }
        }
        panel ed_terrain_reset { width: 100%; layout: column; align: center; padding: 9;
          background: #ff2e1718; border-color: #ff2e1755; border-width: 1; cursor: pointer;
          text { text: "Reset terrain diff"; font-size: 11; color: #ff2e17; } }
        text { text: "LMB drag to sculpt. Shift inverts raise/lower."; font-size: 10; color: #5e5e5e; }
      }
)";
  s += "      panel ed_insp_body { layout: column; align: start; padding: 12 14; gap: 11; width: "
       "100%;\n";
  // Object row.
  s += "        panel { layout: row; align: center; gap: 9; width: 100%;\n"
       "          panel { width: 26; height: 26; background: #000000; layout: "
       "column; justify: center; align: center; " +
       Glyph("cube", TXS) +
       "          }\n"
       "          text ed_insp_name { text: \"\"; font-size: 15; color: #f2f4fb; flex-grow: 1; }\n"
       "          panel { layout: row; align: center; gap: 6;\n"
       "            text { text: \"Static\"; font-size: 11; color: #9a9a9a; }\n"
       "            panel ed_insp_static { width: 14; height: 14; background: "
       "#000000; border-color: #ffffff24; border-width: 1; }\n          }\n        }\n";
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
       "          panel { width: 40; height: 40; background: #000000; overflow: "
       "hidden;"
       " image ed_model_thumb { width: 40; height: 40; } }\n"
       "          panel { layout: row; align: center; gap: 7; flex-grow: 1; background: #000000;"
       " padding: 9 10; border-color: #ffffff14; border-width: 1;\n"
       "            text ed_model_name { text: \"\"; font-size: 12; color: #ffffff; flex-grow: 1; "
       "}\n            " +
       Glyph("folder", TXM) + "          }\n        }\n";
  s += "        panel { layout: row; align: center; gap: 9; width: 100%;\n"
       "          panel { width: 28; height: 28; background: #000000; layout: "
       "column; justify: center; align: center; " +
       Glyph("sphere", TXS) +
       "          }\n"
       "          panel { layout: row; align: center; gap: 7; flex-grow: 1; background: #000000;"
       " padding: 9 10; border-color: #ffffff14; border-width: 1;\n"
       "            text ed_mat_name { text: \"\"; font-size: 12; color: #ffffff; flex-grow: 1; }\n"
       "            text { text: \">\"; font-size: 12; color: #5e5e5e; }\n          }\n        }\n";
  // Details.
  section("Details");
  toggle("Cast Shadow", "ed_tg_cast");
  toggle("Receive Shadow", "ed_tg_recv");
  toggle("Lightmap Static", "ed_tg_lm");
  // Tags.
  section("Tags");
  s += "        panel ed_tags { layout: row; align: center; gap: 6; width: 100%;\n";
  for (int i = 0; i < kEdTags; ++i)
    s += "          panel ed_tag" + base::ToString(i) +
         " { background: #ffffff1f; padding: 5 9;"
         " text ed_tag" +
         base::ToString(i) + "_t { text: \"\"; font-size: 11; color: #ffffff; } }\n";
  s += "          panel { padding: 5; background: #ffffff0c; cursor: pointer; "
       ":hover { background: #ffffff18; } " +
       Glyph("plus", TXM) + "          }\n";
  s += "        }\n";
  s += "      }\n    }\n";  // close ed_insp_body, editor_inspector

  // --- bottom dock: asset browser (width/left overridden each frame in C++) ---
  std::snprintf(
      buf, sizeof(buf),
      "\n    panel editor_browser { position: absolute; left: %g; bottom: %g; width: 1000;"
      " height: %g; layout: column; align: start; background: #0d0d0df8;"
      " border-color: #ffffff12; border-width: 1;\n",
      kEdSceneW, kEdStatusH, kEdBrowserH);
  s += buf;
  // Tab bar.
  s += "      panel { layout: row; align: center; justify: space-between; width: 100%; padding: 0 "
       "12; height: 40;\n"
       "        panel ed_btabs { layout: row; align: center; gap: 2;\n";
  for (int i = 0; i < kEdTabs; ++i)
    s += "          panel btn_btab" + base::ToString(i) +
         " { layout: column; align: center; gap: 6; padding: 9 9; cursor: pointer; background: "
         "#ffffff00;\n"
         "            text btn_btab" +
         base::ToString(i) +
         "_t { text: \"\"; font-size: 12; color: #9a9a9a; }\n"
         "            panel btn_btab" +
         base::ToString(i) +
         "_ul { width: 100%; height: 2; background: #ffffff00; }\n          }\n";
  s += "          panel { padding: 8; background: #ffffff00; cursor: pointer; :hover { background: "
       "#ffffff12; } " +
       Glyph("plus", TXM) + "          }\n        }\n";
  s += "        panel { layout: row; align: center; gap: 12;\n          " + Glyph("magnify", TXM);
  s += "          panel { width: 90; height: 4; background: #3a3a3a; position: "
       "relative;\n"
       "            panel { position: absolute; left: 54; top: -3; width: 10; height: 10; "
       " background: #ffffff; } }\n          " +
       Glyph("grid", TXS);
  s += "          panel ed_cardprev { padding: 5 8; background: #ffffff0c; "
       "cursor: pointer; :hover { background: #ffffff18; } text { text: \"<\"; font-size: 13; "
       "color: #9a9a9a; } }\n";
  s += "          panel ed_cardnext { padding: 5 8; background: #ffffff0c; "
       "cursor: pointer; :hover { background: #ffffff18; } text { text: \">\"; font-size: 13; "
       "color: #9a9a9a; } }\n          " +
       Glyph("kebab", TXM) + "        }\n      }\n";
  s += "      panel { width: 100%; height: 1; background: #ffffff10; }\n";
  // Body: category list + cards.
  s += "      panel { layout: row; align: start; width: 100%; flex-grow: 1;\n"
       "        panel { layout: column; align: start; gap: 2; width: 156; padding: 10 10;\n"
       "          panel ed_asset_search { layout: row; align: center; gap: 7; width: 100%; "
       "background: #000000;"
       " padding: 6 9; border-color: #ffffff14; border-width: 1; cursor: text; "
       "margin: 0 0 6 0;\n            " +
       Glyph("magnify", TXM) +
       "            text ed_asset_search_text { text: \"Search props...\"; font-size: 11; color: "
       "#5e5e5e; flex-grow: 1; }\n"
       "            button ed_asset_clear { text: \"x\"; font-size: 11; color: #5e5e5e; "
       "background: #ffffff00; cursor: pointer; :hover { color: #ffffff; } }\n          }\n";
  for (int i = 0; i < kEdCatRows; ++i)
    s += "          panel cl_row" + base::ToString(i) +
         " { layout: row; align: center; justify: space-between; width: 100%; padding: 5 8;"
         " background: #ffffff00; cursor: pointer; :hover { background: "
         "#ffffff0e; }\n"
         "            text cl_row" +
         base::ToString(i) +
         "_n { text: \"\"; font-size: 12; color: #9a9a9a; }\n"
         "            text cl_row" +
         base::ToString(i) + "_c { text: \"\"; font-size: 11; color: #5e5e5e; }\n          }\n";
  s += "        }\n        panel { width: 1; height: 100%; background: #ffffff0c; }\n";
  s += "        panel ed_cards { layout: row; align: start; gap: 12; flex-grow: 1; padding: 12 14; "
       "overflow: hidden;\n";
  for (int i = 0; i < kEdCards; ++i) {
    const base::String id = base::ToString(i);
    s += "          panel card" + id +
         " { layout: column; align: center; gap: 6; width: 86; cursor: pointer;\n"
         "            panel card" +
         id +
         "_box { width: 86; height: 86; background: #1f232e;"
         " border-color: #ffffff14; border-width: 1; overflow: hidden; position: relative;\n"
         "              panel card" +
         id +
         "_sw { position: absolute; left: 18; top: 18; width: 50; height: 50; "
         "background: #3a3a3a; }\n"
         "              image card" +
         id +
         "_img { position: absolute; left: 0; top: 0; width: 86; height: 86; }\n            }\n"
         "            text card" +
         id +
         "_name { text: \"\"; font-size: 11; color: #9a9a9a; text-align: center; width: 86; }\n    "
         "      }\n";
  }
  s += "        }\n      }\n    }\n";  // close cards, body, editor_browser

  // --- status bar ---
  std::snprintf(buf, sizeof(buf),
                "\n    panel editor_status { position: absolute; left: 0; bottom: 0; width: 100vw;"
                " height: %g; layout: row; align: center; justify: space-between; padding: 0 14;"
                " background: #101010f8; border-color: #ffffff12; border-width: 1;\n",
                kEdStatusH);
  s += buf;
  s += R"(      panel { layout: row; align: center; gap: 8;
        panel { width: 8; height: 8; background: #46c463; }
        text ed_status_left { text: "Ready"; font-size: 12; color: #9a9a9a; }
      }
      panel { layout: row; align: center; gap: 18;
        panel { layout: row; align: center; gap: 7;
          text { text: "Grid"; font-size: 11; color: #5e5e5e; }
          panel ed_grid { layout: row; align: center; gap: 5; background: #000000;
            padding: 4 8; border-color: #ffffff14; border-width: 1; cursor: pointer;
)";
  s +=
      "            text ed_grid_t { text: \"1 m\"; font-size: 11; color: #9a9a9a; }\n            " +
      Glyph("caret", TXM) + "          }\n        }\n";
  s += R"(        panel { layout: row; align: center; gap: 7;
          text { text: "Snapping"; font-size: 11; color: #5e5e5e; }
          panel ed_snap { width: 32; height: 16; background: #3a3a3a; position: relative; cursor: pointer;
            panel ed_snap_k { position: absolute; left: 2; top: 2; width: 12; height: 12; background: #ffffff; } }
        }
        text ed_fps { text: "fps 60"; font-size: 11; color: #9a9a9a; }
      }
    }
  }
)";
  return s;
}

// The character-creation overlay: two absolute-positioned docks whose pixel
// geometry matches the kCg* constants (game_ui.h) so CharGen can hit-test its own
// widgets against the raw cursor. Left dock = race list + sex + preset + page
// tabs + actions; right dock = the pooled slider/cycler rows for the active page.
// Everything is pooled (fixed widget counts, filled and toggled each frame) and
// starts hidden; the engine collapses cg_root until chargen is entered.



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
  if (!editor.active)
    return;

  auto setText = [&](const base::String& n, const base::String& t) {
    SetText(n.c_str(), t.c_str());
  };
  auto setLeft = [&](const base::String& n, float v) {
    SetStyleField(
        n.c_str(), [](ugui::Style& s, float v) { s.left_offset = ugui::Length::Px(v); }, v);
  };
  auto setWidth = [&](const base::String& n, float v) {
    SetStyleField(n.c_str(), [](ugui::Style& s, float v) { s.width = ugui::Length::Px(v); }, v);
  };

  // Keep the bottom browser spanning between the side docks at any window width.
  const float bw = host.window_width - kEdSceneW - kEdInspectorW;
  setLeft("editor_browser", kEdSceneW);
  setWidth("editor_browser", bw > 200.0f ? bw : 200.0f);

  // Toolbar: highlight the active tool; gizmo bar mirrors the gizmo mode.
  for (int i = 0; i < kEdToolBtns; ++i) {
    const base::String id = "btn_tool" + base::ToString(i);
    const bool on = i == editor.tool;
    SetBackground(id.c_str(), on ? kEdAccentSoft : kEdClear);
    SetBackground((id + "_ul").c_str(), on ? kEdAccent : kEdClear);
    SetTextColor((id + "_lbl").c_str(), on ? kEdTxP : kEdTxS);
  }
  for (int i = 0; i < 4; ++i)
    SetBackground(("btn_giz" + base::ToString(i)).c_str(),
                  i == editor.gizmo ? kEdAccentSoft : kEdClear);

  // Left dock tabs.
  SetTextColor("btn_ltab0_t", editor.left_tab == 0 ? kEdTxP : kEdTxM);
  SetTextColor("btn_ltab1_t", editor.left_tab == 1 ? kEdTxP : kEdTxM);
  SetBackground("btn_ltab0_ul", editor.left_tab == 0 ? kEdAccent : kEdClear);
  SetBackground("btn_ltab1_ul", editor.left_tab == 1 ? kEdAccent : kEdClear);

  // Scene tree search box.
  {
    const bool ph = editor.scene_search.empty() && !editor.scene_search_focused;
    setText(
        "ed_scene_search_text",
        ph ? "Search scene..." : editor.scene_search + (editor.scene_search_focused ? "|" : ""));
    SetTextColor("ed_scene_search_text", ph ? kEdTxM : kEdTxP);
  }

  // Scene hierarchy tree rows.
  for (int i = 0; i < kEdTreeRows; ++i) {
    const base::String row = "ed_trow" + base::ToString(i);
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
  SetVisible("ed_insp_empty", !editor.has_selection && !editor.terrain_mode);
  SetVisible("ed_insp_body", editor.has_selection && !editor.terrain_mode);
  SetVisible("ed_terrain_body", editor.terrain_mode);
  if (editor.terrain_mode) {
    static const char* mode_ids[] = {"ed_terrain_mode0", "ed_terrain_mode1", "ed_terrain_mode2",
                                     "ed_terrain_mode3"};
    for (int i = 0; i < 4; ++i) {
      const bool active = i == editor.terrain_brush_mode;
      SetBackground(mode_ids[i], active ? kEdAccentSoft : kEdField);
      SetTextColor((base::String(mode_ids[i]) + "_t").c_str(), active ? kEdTxP : kEdTxS);
    }
    char value[64];
    std::snprintf(value, sizeof(value), "%.1f m", editor.terrain_radius);
    setText("ed_terrain_radius", value);
    std::snprintf(value, sizeof(value), "%.2f", editor.terrain_strength);
    setText("ed_terrain_strength", value);
    setText("ed_terrain_samples", base::ToString(editor.terrain_sample_count));
    setText("ed_terrain_dirty", editor.terrain_dirty ? "Unsaved" : "Saved");
    SetBackground("ed_terrain_dirty_chip",
                  editor.terrain_dirty ? Rgba(0xe8b54a2eu) : Rgba(0x46c46324u));
    SetTextColor("ed_terrain_dirty", editor.terrain_dirty ? Rgba(0xf0c86affu) : Rgba(0x70d88affu));
    base::String path = editor.terrain_path;
    if (path.size() > 38)
      path = "..." + path.substr(path.size() - 35);
    setText("ed_terrain_path", path.empty() ? "editor_layout.recterrain" : path);
  }
  if (editor.has_selection && !editor.terrain_mode) {
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
    ugui::SetImageTexture(Need("ed_model_thumb"), editor.model_thumb,
                          editor.model_thumb ? 40.0f : 0.0f, editor.model_thumb ? 40.0f : 0.0f);
    SetBackground("ed_insp_static", editor.sel_static ? kEdAccentSoft : kEdField);
    auto toggle = [&](const char* name, bool on) {
      SetBackground(name, on ? kEdAccent : kEdToggleOff);
      setLeft(base::String(name) + "_k", on ? 18.0f : 2.0f);
    };
    toggle("ed_tg_cast", editor.cast_shadow);
    toggle("ed_tg_recv", editor.receive_shadow);
    toggle("ed_tg_lm", editor.lightmap_static);
    for (int i = 0; i < kEdTags; ++i) {
      const base::String t = "ed_tag" + base::ToString(i);
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
    const base::String id = "btn_btab" + base::ToString(i);
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
    setText(
        "ed_asset_search_text",
        ph ? "Search props..." : editor.asset_search + (editor.asset_search_focused ? "|" : ""));
    SetTextColor("ed_asset_search_text", ph ? kEdTxM : kEdTxP);
  }

  // Category list with counts.
  for (int i = 0; i < kEdCatRows; ++i) {
    const base::String id = "cl_row" + base::ToString(i);
    if (i < static_cast<int>(editor.cats.size())) {
      SetVisible(id.c_str(), true);
      setText(id + "_n", editor.cats[i].name);
      setText(id + "_c", base::ToString(editor.cats[i].count));
      const bool on = editor.cats[i].active;
      SetBackground(id.c_str(), on ? kEdAccentSoft : kEdClear);
      SetTextColor((id + "_n").c_str(), on ? kEdTxP : kEdCat);
    } else {
      SetVisible(id.c_str(), false);
    }
  }

  // Asset cards.
  for (int i = 0; i < kEdCards; ++i) {
    const base::String id = "card" + base::ToString(i);
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
    const float x0 = base::Min(editor.marquee[0], editor.marquee[2]);
    const float y0 = base::Min(editor.marquee[1], editor.marquee[3]);
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
    SetStyleField(
        "ed_select", [](ugui::Style& s, float v) { s.top = ugui::Length::Px(v); },
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
  if (!editor_sink || !editor.active)
    return false;
  // Climb from the clicked widget (the deepest hit) to the nearest editor-handled
  // name. Tree rows distinguish the eye / expand children by name suffix.
  ugui::wid w = target;
  for (int depth = 0; depth < 8 && w.valid(); ++depth) {
    const ugui::WidgetNode* n = ui.world().Get<ugui::WidgetNode>(w);
    if (n) {
      const base::String name = n->name.c_str();
      auto pref = [&](const char* p) -> int {
        const size_t pl = std::strlen(p);
        if (name.size() >= pl && name.compare(0, pl, p) == 0)
          return std::atoi(name.c_str() + pl);
        return -1;
      };
      auto has = [&](const char* sub) { return name.find(sub) != base::String::npos; };
      using K = EditorUiEvent::Kind;
      EditorUiEvent e;
      if (int i = pref("ed_trow"); i >= 0) {
        e.index = i;
        e.kind = has("_eye") ? K::kTreeEye : has("_exp") ? K::kTreeExpand : K::kTreeSelect;
        editor_sink(e);
        return true;
      }
      if (int i = pref("btn_tool"); i >= 0) {
        e.kind = K::kTool;
        e.index = i;
        editor_sink(e);
        return true;
      }
      if (int i = pref("btn_giz"); i >= 0) {
        e.kind = K::kGizmo;
        e.index = i;
        editor_sink(e);
        return true;
      }
      if (int i = pref("btn_ltab"); i >= 0) {
        e.kind = K::kLeftTab;
        e.index = i;
        editor_sink(e);
        return true;
      }
      if (int i = pref("btn_btab"); i >= 0) {
        e.kind = K::kCategory;
        e.index = i;
        editor_sink(e);
        return true;
      }
      if (int i = pref("cl_row"); i >= 0) {
        e.kind = K::kCategory;
        e.index = i;
        editor_sink(e);
        return true;
      }
      if (int i = pref("card"); i >= 0) {
        e.kind = K::kPickCard;
        e.index = i;
        editor_sink(e);
        return true;
      }
      if (name == "ed_scene_clear") {
        e.kind = K::kClearScene;
        editor_sink(e);
        return true;
      }
      if (name == "ed_scene_search" || name == "ed_scene_search_text") {
        e.kind = K::kFocusScene;
        editor_sink(e);
        return true;
      }
      if (name == "ed_asset_clear") {
        e.kind = K::kClearAsset;
        editor_sink(e);
        return true;
      }
      if (name == "ed_asset_search" || name == "ed_asset_search_text") {
        e.kind = K::kFocusAsset;
        editor_sink(e);
        return true;
      }
      if (name == "ed_cardprev") {
        e.kind = K::kCardScroll;
        e.index = -1;
        editor_sink(e);
        return true;
      }
      if (name == "ed_cardnext") {
        e.kind = K::kCardScroll;
        e.index = 1;
        editor_sink(e);
        return true;
      }
      if (name == "btn_treeup") {
        e.kind = K::kTreeScroll;
        e.index = -1;
        editor_sink(e);
        return true;
      }
      if (name == "btn_treedn") {
        e.kind = K::kTreeScroll;
        e.index = 1;
        editor_sink(e);
        return true;
      }
      if (name == "ed_snap") {
        e.kind = K::kSnapToggle;
        editor_sink(e);
        return true;
      }
      if (name == "ed_grid") {
        e.kind = K::kGridCycle;
        editor_sink(e);
        return true;
      }
      if (int i = pref("ed_terrain_mode"); i >= 0) {
        e.kind = K::kTerrainMode;
        e.index = i;
        editor_sink(e);
        return true;
      }
      if (name == "ed_terrain_radius_dec" || name == "ed_terrain_radius_inc") {
        e.kind = K::kTerrainRadius;
        e.index = has("_dec") ? -1 : 1;
        editor_sink(e);
        return true;
      }
      if (name == "ed_terrain_strength_dec" || name == "ed_terrain_strength_inc") {
        e.kind = K::kTerrainStrength;
        e.index = has("_dec") ? -1 : 1;
        editor_sink(e);
        return true;
      }
      if (name == "ed_terrain_reset") {
        e.kind = K::kTerrainReset;
        editor_sink(e);
        return true;
      }
    }
    const ugui::Hierarchy* h = ui.world().Get<ugui::Hierarchy>(w);
    w = h ? h->parent : ugui::wid{};
  }
  return false;
}


}  // namespace rx

#endif  // RECREATION_HAS_UGUI
