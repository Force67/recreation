// Assembly of the ugui document: which .ugui fragments load, in what order,
// and which fonts back them.

#include "runtime/ui/game_ui_internal.h"

#if defined(RECREATION_HAS_UGUI)

namespace rx {

const char* const kUiFragments[kUiFragmentCount] = {
    "theme.ugui",     "hud.ugui",        "vitals.ugui",     "readout.ugui",
    "quest.ugui",     "hud_gauge.ugui",  "location.ugui",   "chat.ugui",
    "scoreboard.ugui",
    "mp_prompt.ugui", "nametag.ugui",    "journal.ugui",    "war_map.ugui",
    "player_map.ugui",
    "dialogue.ugui",  "container.ugui",  "pause_menu.ugui", "main_menu.ugui",
    "first_run.ugui", "loading.ugui",  "legal.ugui",
};

// Directory holding the .ugui fragments: RECREATION_UI_DIR, else the compiled-in
// source path, else a cwd-relative fallback.
fs::path UiDir() {
  if (const char* env = UiDirOpt.get(); env && *env)
    return env;
#ifdef RECREATION_UI_DIR_DEFAULT
  return fs::path(RECREATION_UI_DIR_DEFAULT);
#else
  return fs::path("runtime/ui");
#endif
}

// Read one .ugui fragment. Returns its text, or "" (with a warning) if missing.
base::String LoadUiFragment(const char* name) {
  const fs::path p = UiDir() / name;
  std::ifstream f(p.c_str(), std::ios::binary);
  if (!f) {
    RX_WARN("ui: fragment not found: {}", p.string());
    return {};
  }
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Loaded once and kept, so the image manifests survive a hot reload of the
// hand-written fragments.
base::Vector<ui::VanillaScreen>& VanillaScreens() {
  static base::Vector<ui::VanillaScreen> screens;
  static bool loaded = false;
  if (loaded)
    return screens;
  loaded = true;
  const base::String dir = ui::VanillaScreenDir();
  for (const base::String& name : ui::VanillaScreenNames()) {
    ui::VanillaScreen screen;
    if (ui::LoadVanillaScreen(dir, name, screen))
      screens.push_back(base::move(screen));
  }
  if (!screens.empty())
    RX_INFO("ui: {} vanilla screen(s) from {}", screens.size(), dir);
  return screens;
}

VanillaRole VanillaRoleOf(base::StringRef name) {
  if (name == "startmenu" || name == "mainmenu")
    return VanillaRole::kFrontMenu;  // Skyrim's boot menu, Fallout 4's
  if (name == "quest_journal")
    return VanillaRole::kPauseMenu;  // its System page is what Esc opens
  return VanillaRole::kHud;
}

base::String VanillaRootName(base::StringRef screen) {
  return "vanilla_" + base::String(screen);
}

bool UsingVanillaUi() {
  return !VanillaScreens().empty();
}

void BindVanillaScreens(ugui::UIContext& context, ugui::TextureBackend& backend) {
  base::Vector<ui::VanillaScreen>& screens = VanillaScreens();
  if (screens.empty())
    return;
  ui::ReleaseVanillaImages(backend);  // a rebuild re-uploads them all
  const base::String dir = ui::VanillaScreenDir();
  u32 bound = 0;
  for (const ui::VanillaScreen& screen : screens)
    bound += ui::BindVanillaImages(context, backend, dir, screen);
  RX_INFO("ui: bound {} vanilla image(s)", bound);
}

// The scrolling compass: letters only, no rule and no pips. Generated per
// index since cardinals and intercardinals differ in size and value. Heading
// reads off the centre of the window, which is screen centre, so nothing marks
// it. The 70%/24% split lets the strip fade at its own edges without a gradient
// overlay, which it could not have anyway with no plate behind it.
base::String BuildTopbarSection() {
  base::String s = R"(
  panel topbar {
    position: absolute; top: 0; left: 0; width: 100vw; height: 68;
    layout: row; justify: center; align: start; padding: 46 0 0 0;
    panel compass_window {
      width: 380; height: 22; position: relative; overflow: hidden;
      panel compass_strip {
        position: absolute; top: 0; left: 0; height: 22; width: 1800;
        layout: row; align: center;
)";
  const int count = 8 * kCompassTurns;
  for (int i = 0; i < count; ++i) {
    const char* card = kCardinals[i % 8];
    const bool major = (i % 2) == 0;  // N E S W
    s += "        text cl" + base::ToString(i) + " { text: \"" + card +
         "\"; width: 75; text-align: center; font-size: " +
         base::ToString(major ? 11 : 8) +
         "; letter-spacing: " + base::ToString(major ? 3 : 2) +
         "; color: " + (major ? "#ffffffb3" : "#ffffff3d") +
         "; text-shadow-color: #000000cc; text-shadow-x: 1; text-shadow-y: 1; }\n";
  }
  s += R"(
      }
    }
  }
)";
  return s;
}

// Assemble the whole UI tree: the root, the procedural sections generated in
// code (topbar, editor, main menu), and the static screens loaded from the
// .ugui fragments, all siblings of root in draw order. The pause/front menus are
// concatenated last so they overlay everything.
base::String BuildUi() {
  // theme.ugui declares the shared style classes and components. They are
  // top-level constructs, so it has to lead the document, outside root.
  base::String s = LoadUiFragment("theme.ugui");
  s += "\npanel root {\n  width: 100vw; height: 100vh; position: relative;\n";
  s += BuildTopbarSection();        // procedural: scrolling compass
  s += LoadUiFragment("hud.ugui");  // crosshair
  s += LoadUiFragment("vitals.ugui");
  s += LoadUiFragment("readout.ugui");
  s += LoadUiFragment("quest.ugui");
  s += LoadUiFragment("hud_gauge.ugui");
  s += LoadUiFragment("location.ugui");
  s += LoadUiFragment("chat.ugui");
  s += LoadUiFragment("scoreboard.ugui");
  s += LoadUiFragment("mp_prompt.ugui");
  s += LoadUiFragment("nametag.ugui");
  s += LoadUiFragment("journal.ugui");
  s += LoadUiFragment("war_map.ugui");
  s += LoadUiFragment("player_map.ugui");
  s += LoadUiFragment("dialogue.ugui");
  s += LoadUiFragment("container.ugui");
  s += BuildEditorSection();   // procedural: Glyph icons; before the menu
  s += BuildCharGenSection();  // procedural: character creation docks
  s += LoadUiFragment("pause_menu.ugui");
  s += LoadUiFragment("main_menu.ugui");
  s += LoadUiFragment("first_run.ugui");  // out-of-box wizard, overlays the menu
  s += LoadUiFragment("loading.ugui");    // covers everything while a world loads
  for (const ui::VanillaScreen& screen : VanillaScreens())
    s += screen.markup;               // translated Scaleform, on top of everything
  s += LoadUiFragment("legal.ugui");  // the startup notice, over all of it
  s += "}\n";
  return s;
}


const char* FindFont() {
  static base::String resolved;
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
      base::String path(buf);
      while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
        path.pop_back();
      if (!path.empty() && fs::exists(path.c_str())) {
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
  static base::String resolved;
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
      base::String path(buf);
      while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
        path.pop_back();
      if (!path.empty() && fs::exists(path.c_str())) {
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

// A static bold face, selectable in markup as `font: bold`.
//
// `fc-match 'sans:style=Bold'` is a trap: it returns variable fonts such as
// NotoSans.ttf, of which FreeType silently renders only the default (regular)
// instance. Ask for families shipping real static bolds and reject any answer
// whose filename does not say Bold.
const char* FindBoldFont() {
  static base::String resolved;
  if (const char* env = UiFontBold.get(); env && fs::exists(env)) {
    resolved = env;
    return resolved.c_str();
  }
#if !defined(_WIN32)
  static const char* queries[] = {
      "Liberation Sans:style=Bold",
      "DejaVu Sans:style=Bold",
      "Arimo:style=Bold",
  };
  for (const char* q : queries) {
    base::String cmd = "fc-match -f '%{file}' '";
    cmd += q;
    cmd += "' 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) continue;
    char buf[1024];
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, p);
    pclose(p);
    if (n == 0) continue;
    buf[n] = '\0';
    base::String path(buf);
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
      path.pop_back();
    // Guard against the variable-font substitution described above.
    if (path.find("Bold") == base::String::npos &&
        path.find("bold") == base::String::npos)
      continue;
    if (!path.empty() && fs::exists(path.c_str())) {
      resolved = path;
      return resolved.c_str();
    }
  }
#endif
  static const char* candidates[] = {
#if defined(_WIN32)
      "C:/Windows/Fonts/segoeuib.ttf",
      "C:/Windows/Fonts/arialbd.ttf",
#else
      "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
      "/usr/share/fonts/TTF/LiberationSans-Bold.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
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


}  // namespace rx

#endif  // RECREATION_HAS_UGUI
