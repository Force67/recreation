#include "engine.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include <base/option.h>

#include "bethesda/game_profile.h"
#include "core/log.h"
#include "engine_internal.h"

#ifdef _WIN32
// MSVC names the pipe helpers _popen/_pclose; the folder picker shells out the
// same way on every platform (PowerShell on Windows).
static FILE* popen(const char* cmd, const char* mode) { return _popen(cmd, mode); }
static int pclose(FILE* stream) { return _pclose(stream); }
#endif

// The first-run / out-of-box setup wizard: the front door a fresh install opens
// before the NEXUS main menu. Pre-resolves the installed universes, lets the
// player point Recreation at any it could not find, pick a mods directory and a
// few preferences, then persists the choices to a small setup.ini and hands off
// to SetupMainMenu. A marker in that file (done=1) suppresses the wizard on
// every later launch, and the player can re-run it from the menu's Settings.
namespace rec {
namespace fs = std::filesystem;

namespace {

// Test/CI hooks, formerly read straight from the environment. Namespace scope so
// they register before InitOptionsFromEnv() runs at startup.
base::Option<const char*> PickOverride{"pick.override", nullptr, "REC_PICK_OVERRIDE"};
base::Option<bool> ForceFirstRun{"force.first.run", false, "REC_FORCE_FIRST_RUN"};
base::Option<const char*> FirstrunAutobrowse{"firstrun.autobrowse", nullptr,
                                             "REC_FIRSTRUN_AUTOBROWSE"};
base::Option<bool> FirstrunAutolaunch{"firstrun.autolaunch", false, "REC_FIRSTRUN_AUTOLAUNCH"};

// Per-platform user config directory holding setup.ini and the default mods
// folder: %APPDATA%\Recreation, ~/Library/Application Support/Recreation, or
// $XDG_CONFIG_HOME/recreation (~/.config/recreation).
fs::path SetupDir() {
#if defined(_WIN32)
  if (const char* a = std::getenv("APPDATA")) return fs::path(a) / "Recreation";
  return fs::path("Recreation");
#elif defined(__APPLE__)
  if (const char* h = std::getenv("HOME"))
    return fs::path(h) / "Library" / "Application Support" / "Recreation";
  return fs::path("Recreation");
#else
  if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return fs::path(x) / "recreation";
  if (const char* h = std::getenv("HOME")) return fs::path(h) / ".config" / "recreation";
  return fs::path(".recreation");
#endif
}

fs::path SetupFile() { return SetupDir() / "setup.ini"; }
std::string DefaultModsDir() { return (SetupDir() / "mods").string(); }

// The three wizard columns, in order, with the env/persist keys and the nicer
// display name shown on the locate page.
struct GameSpec {
  bethesda::Game game;
  const char* key;       // setup.ini key
  const char* display;   // locate-page label
};
const GameSpec kGameSpecs[3] = {
    {bethesda::Game::kSkyrimSe, "skyrim_data", "Skyrim Special Edition"},
    {bethesda::Game::kFallout4, "fallout4_data", "Fallout 4"},
    {bethesda::Game::kStarfield, "starfield_data", "Starfield"},
};

// Parse setup.ini into a flat key=value map. Missing file -> empty map.
std::map<std::string, std::string> ReadIni() {
  std::map<std::string, std::string> kv;
  std::ifstream f(SetupFile());
  if (!f) return kv;
  std::string line;
  while (std::getline(f, line)) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
    kv[k] = v;
  }
  return kv;
}

// Open a native folder picker and return the chosen absolute path ("" if the
// user cancelled or no picker is available). Uses the same shell-out approach as
// the menu's OpenUrl: zenity/kdialog on Linux, NSOpenPanel via osascript on
// macOS, a FolderBrowserDialog via PowerShell on Windows.
std::string RunPicker(const std::string& cmd) {
  FILE* p = popen(cmd.c_str(), "r");
  if (!p) return "";
  std::string out;
  char buf[1024];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
  pclose(p);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  return out;
}

std::string PickFolder(const std::string& title) {
  // Test hook: skip the GUI dialog and return a fixed path. Lets the browse flow
  // run without a display (headless capture, CI).
  if (const char* o = PickOverride.get()) return o;
#if defined(_WIN32)
  const std::string cmd =
      "powershell -NoProfile -Command \"Add-Type -AssemblyName System.Windows.Forms; "
      "$f=New-Object System.Windows.Forms.FolderBrowserDialog; $f.Description='" +
      title + "'; if($f.ShowDialog() -eq 'OK'){Write-Output $f.SelectedPath}\"";
  return RunPicker(cmd);
#elif defined(__APPLE__)
  return RunPicker("osascript -e 'POSIX path of (choose folder with prompt \"" + title +
                   "\")' 2>/dev/null");
#else
  std::string p = RunPicker("zenity --file-selection --directory --title=\"" + title +
                            "\" 2>/dev/null");
  if (p.empty())
    p = RunPicker("kdialog --getexistingdirectory \"$HOME\" --title \"" + title + "\" 2>/dev/null");
  return p;
#endif
}

// Resolve a user-picked folder to a valid Data directory for `game`, or "" if
// neither the folder nor its Data subfolder holds that game's master plugin.
// Players sometimes pick the game root rather than its Data folder, so both are
// checked; the returned path is the one that actually contains the master.
std::string ResolvePickedDataDir(bethesda::Game game, const std::string& picked) {
  const auto& profile = bethesda::GameProfile::For(game);
  if (profile.base_masters.empty()) return picked;  // unknown game: accept as-is
  const std::string master(profile.base_masters[0].c_str());
  std::error_code ec;
  if (fs::exists(fs::path(picked) / master, ec)) return picked;
  const fs::path data = fs::path(picked) / "Data";
  if (fs::exists(data / master, ec)) return data.string();
  return "";
}

}  // namespace

// Pull any persisted game paths and mods directory into the EngineConfig so a
// later launch resolves the same universes the wizard found (ResolveUniverses
// reads config_.extra_domains). Safe to call even when no setup.ini exists.
void LoadSetupConfig(Engine& engine) {
  Engine* const self = &engine;
  const std::map<std::string, std::string> kv = ReadIni();
  if (kv.empty()) return;
  for (const GameSpec& spec : kGameSpecs) {
    const auto it = kv.find(spec.key);
    if (it == kv.end() || it->second.empty()) continue;
    ExtraDomainConfig d;
    d.game = spec.game;
    d.data_dir = it->second;
    d.plugins_txt = it->second + "/../plugins.txt";
    self->config_.extra_domains.push_back(d);
  }
  if (const auto it = kv.find("mods_dir"); it != kv.end() && !it->second.empty()) {
    self->config_.mods_dir = it->second;
    self->first_run_mods_dir_ = it->second;
  }
}

// True once the wizard has been completed (setup.ini exists with done=1).
// REC_FORCE_FIRST_RUN forces the wizard back on for testing.
bool FirstRunComplete() {
  if (ForceFirstRun) return false;
  const auto kv = ReadIni();
  const auto it = kv.find("done");
  return it != kv.end() && it->second == "1";
}

void SetupFirstRun(Engine& engine) {
  Engine* const self = &engine;
  self->first_run_active_ = true;
  ResolveUniverses(engine);  // pre-detect installed games (config / env / Steam)
  if (self->first_run_mods_dir_.empty()) self->first_run_mods_dir_ = DefaultModsDir();
  self->game_ui_.OpenFirstRun();
  self->debug_ui_.SetVisible(false);  // a clean front screen, no debug overlays
  REC_INFO("first-run setup wizard open");
}

namespace {

// Persist the wizard's choices and the done marker that suppresses it on later
// launches. data_dirs holds each column's located path ("" if not found); the
// privileged caller (Engine::UpdateFirstRun) gathers it from menu_universes_.
void WriteSetupIni(const std::array<std::string, 3>& data_dirs, const std::string& mods_dir,
                   const FirstRunRequest& r) {
  std::error_code ec;
  fs::create_directories(SetupDir(), ec);
  std::ofstream f(SetupFile(), std::ios::trunc);
  if (!f) {
    REC_WARN("first-run: could not write {}", SetupFile().string());
    return;
  }
  f << "done=1\n";
  for (int i = 0; i < 3; ++i)
    if (!data_dirs[i].empty()) f << kGameSpecs[i].key << "=" << data_dirs[i] << "\n";
  f << "mods_dir=" << (mods_dir.empty() ? DefaultModsDir() : mods_dir) << "\n";
  f << "default_mode=" << r.mode << "\n";
  f << "difficulty=" << r.difficulty << "\n";
  f << "enable_mods=" << (r.enable_mods ? 1 : 0) << "\n";
  f << "share_diagnostics=" << (r.share_diagnostics ? 1 : 0) << "\n";
  f << "check_updates=" << (r.check_updates ? 1 : 0) << "\n";
  REC_INFO("first-run setup saved to {}", SetupFile().string());
}

}  // namespace

void Engine::UpdateFirstRun(f32 dt) {
  (void)dt;
  window_->SetRelativeMouseMode(false);  // free cursor so the wizard can be clicked

  // Validate a picked folder to game `idx`'s Data dir and mark it available.
  // Shared by the browse click and the auto-browse test hook below.
  auto accept_folder = [this](int idx, const std::string& picked) {
    if (idx < 0 || idx >= 3 || picked.empty()) return;
    MenuUniverse& u = menu_universes_[idx];
    const std::string data = ResolvePickedDataDir(u.game, picked);
    if (data.empty()) {
      REC_WARN("first-run: {} not found under {}", u.name, picked);
      return;
    }
    u.data_dir = data;
    u.plugins_txt = data + "/../plugins.txt";
    u.available = true;
    REC_INFO("first-run: located {} at {}", u.name, data);
  };

  // Test hook: REC_FIRSTRUN_AUTOBROWSE=<0..2> browses that column once on the
  // first frame (mirrors REC_MENU_AUTOPLAY), so the picker, validation and
  // locate path run without a mouse. Pair with REC_PICK_OVERRIDE for the folder.
  if (const char* ab = FirstrunAutobrowse.get()) {
    static bool fired = false;
    if (!fired) {
      fired = true;
      accept_folder(std::atoi(ab), PickFolder("auto"));
    }
  }

  // Keyboard conveniences: Accept advances the page (the primary button), Cancel
  // steps back / cancels at the first page. The mouse drives everything else.
  if (actions_.pressed(Action::kMenuAccept)) game_ui_.FirstRunNext();
  if (actions_.pressed(Action::kMenuCancel)) game_ui_.FirstRunBack();

  // Mirror the resolved games + chosen mods dir into the wizard each frame.
  FirstRunView view;
  for (int i = 0; i < 3; ++i) {
    FirstRunView::Game g;
    g.name = kGameSpecs[i].display;
    g.path = menu_universes_[i].data_dir;
    g.located = menu_universes_[i].available;
    view.games.push_back(g);
  }
  view.mods_dir = first_run_mods_dir_;
  game_ui_.SetFirstRunView(view);

  // Test hook: REC_FIRSTRUN_AUTOLAUNCH advances one page per frame to the end and
  // launches, so the setup->main-menu handoff can be verified headlessly. Runs
  // after the view push so the locate-page gate sees any auto-browsed game.
  if (FirstrunAutolaunch) game_ui_.FirstRunNext();

  const FirstRunRequest req = game_ui_.PollFirstRunRequest();
  switch (req.kind) {
    case FirstRunRequest::Kind::kBrowseGame: {
      if (req.index < 0 || req.index >= 3) break;
      accept_folder(req.index,
                    PickFolder("Locate the " + menu_universes_[req.index].name + " Data folder"));
      break;
    }
    case FirstRunRequest::Kind::kBrowseMods: {
      const std::string p = PickFolder("Choose the Recreation mods directory");
      if (!p.empty()) first_run_mods_dir_ = p;
      break;
    }
    case FirstRunRequest::Kind::kLaunch: {
      // Fold the located universes into config_.extra_domains so the SetupMainMenu
      // that follows re-resolves to the same paths, and gather them for the ini.
      std::array<std::string, 3> data_dirs;
      for (int i = 0; i < 3; ++i) {
        const MenuUniverse& u = menu_universes_[i];
        if (!u.available || u.data_dir.empty()) continue;
        data_dirs[i] = u.data_dir;
        ExtraDomainConfig d;
        d.game = kGameSpecs[i].game;
        d.data_dir = u.data_dir;
        d.plugins_txt = u.plugins_txt.empty() ? (u.data_dir + "/../plugins.txt") : u.plugins_txt;
        config_.extra_domains.push_back(d);
      }
      config_.mods_dir = first_run_mods_dir_.empty() ? DefaultModsDir() : first_run_mods_dir_;
      WriteSetupIni(data_dirs, config_.mods_dir, req);
      first_run_active_ = false;
      game_ui_.CloseFirstRun();
      SetupMainMenu(*this);  // hand off to the normal front screen
      break;
    }
    case FirstRunRequest::Kind::kCancel: {
      // Skip setup for now (do not write the done marker, so it returns next
      // launch) and drop straight into the main menu.
      first_run_active_ = false;
      game_ui_.CloseFirstRun();
      SetupMainMenu(*this);
      break;
    }
    case FirstRunRequest::Kind::kNone:
      break;
  }
}

}  // namespace rec
