#include <base/containers/array.h>
#include <base/containers/map.h>
#include <base/option.h>
#include <base/strings/xstring.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <SDL3/SDL.h>

#include "components/bethesda/game_profile.h"
#include "core/log.h"
#include "runtime/app/engine.h"
#include "runtime/app/engine_internal.h"

// The first-run / out-of-box setup wizard: the front door a fresh install opens
// before the NEXUS main menu. Pre-resolves the installed universes, lets the
// player point Recreation at any it could not find, pick a mods directory and a
// few preferences, then persists the choices to a small setup.ini and hands off
// to SetupMainMenu. A marker in that file (done=1) suppresses the wizard on
// every later launch, and the player can re-run it from the menu's Settings.
namespace rx {
namespace fs = std::filesystem;

namespace {

// Test/CI hooks, formerly read straight from the environment. Namespace scope so
// they register before InitOptionsFromEnv() runs at startup.
base::Option<const char*> PickOverride{"pick.override", nullptr, "RX_PICK_OVERRIDE"};
base::Option<bool> ForceFirstRun{"force.first.run", false, "RX_FORCE_FIRST_RUN"};
base::Option<const char*> FirstrunAutobrowse{"firstrun.autobrowse", nullptr,
                                             "RX_FIRSTRUN_AUTOBROWSE"};
base::Option<bool> FirstrunAutolaunch{"firstrun.autolaunch", false, "RX_FIRSTRUN_AUTOLAUNCH"};

// Per-platform user config directory holding setup.ini and the default mods
// folder: %APPDATA%\Recreation, ~/Library/Application Support/Recreation, or
// $XDG_CONFIG_HOME/recreation (~/.config/recreation).
fs::path SetupDir() {
#if defined(_WIN32)
  if (const char* a = std::getenv("APPDATA"))
    return fs::path(a) / "Recreation";
  return fs::path("Recreation");
#elif defined(__APPLE__)
  if (const char* h = std::getenv("HOME"))
    return fs::path(h) / "Library" / "Application Support" / "Recreation";
  return fs::path("Recreation");
#else
  if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x)
    return fs::path(x) / "recreation";
  if (const char* h = std::getenv("HOME"))
    return fs::path(h) / ".config" / "recreation";
  return fs::path(".recreation");
#endif
}

fs::path SetupFile() {
  return SetupDir() / "setup.ini";
}
base::String DefaultModsDir() {
  return (SetupDir() / "mods").string();
}

// The three wizard columns, in order, with the env/persist keys and the nicer
// display name shown on the locate page.
struct GameSpec {
  bethesda::Game game;
  const char* key;      // setup.ini key
  const char* display;  // locate-page label
};
const GameSpec kGameSpecs[3] = {
    {bethesda::Game::kSkyrimSe, "skyrim_data", "Skyrim Special Edition"},
    {bethesda::Game::kFallout4, "fallout4_data", "Fallout 4"},
    {bethesda::Game::kStarfield, "starfield_data", "Starfield"},
};

// Parse setup.ini into a flat key=value map. Missing file -> empty map.
base::Map<base::String, base::String> ReadIni() {
  base::Map<base::String, base::String> kv;
  std::ifstream f(SetupFile());
  if (!f)
    return kv;
  // std::getline fills a std::string; each line is copied into a base::String.
  std::string source;
  while (std::getline(f, source)) {
    const base::String line(source.c_str(), source.size());
    const mem_size eq = line.find('=');
    if (eq == base::String::npos)
      continue;
    base::String k = line.substr(0, eq), v = line.substr(eq + 1);
    while (!v.empty() && (v.back() == '\r' || v.back() == '\n'))
      v.pop_back();
    kv[k] = v;
  }
  return kv;
}

// The folder picker.
//
// SDL raises the desktop's own dialog: the XDG portal over D-Bus where there is
// one (which is also the only route that works inside a flatpak), zenity where
// there is not, IFileDialog on Windows, NSOpenPanel on macOS. This used to
// popen `zenity --file-selection` with the title pasted into a shell string,
// falling back to kdialog, to osascript, to a PowerShell FolderBrowserDialog --
// which froze the engine for as long as the dialog was open and had nothing to
// say on a desktop that shipped none of them.
//
// It is asynchronous, and it has to be: SDL calls back when the player is done,
// possibly from another thread, so the answer is parked here and UpdateFirstRun
// takes it on a later frame. One dialog at a time is all the wizard can ask
// for, so one slot is enough.
constexpr int kPickModsDir = 3;  // target for the mods directory, 0..2 are games

struct FolderPick {
  std::atomic<bool> ready{false};  // a result is parked; written last, read first
  bool open = false;               // a dialog is up: do not raise a second
  int target = 0;
  base::String path;  // empty means cancelled, or the dialog failed to open
  SDL_PropertiesID props = 0;
};
FolderPick g_pick;

void FolderPicked(void* /*userdata*/, const char* const* filelist, int /*filter*/) {
  // A null list is an error and a list whose first entry is null is a cancel.
  // Neither is worth a word to the player: nothing happened.
  g_pick.path = (filelist && filelist[0]) ? filelist[0] : "";
  g_pick.ready.store(true, std::memory_order_release);
}

void PickFolder(Window& window, int target, const base::String& title) {
  if (g_pick.open || g_pick.ready.load(std::memory_order_acquire))
    return;  // one at a time, and never over an answer nobody has read yet
  g_pick.target = target;
  // Test hook: answer with a fixed path instead of opening anything, so the
  // browse flow runs without a display (headless capture, CI).
  if (const char* o = PickOverride.get()) {
    g_pick.path = o;
    g_pick.ready.store(true, std::memory_order_release);
    return;
  }
  g_pick.props = SDL_CreateProperties();
  if (g_pick.props == 0) {
    RX_WARN("first-run: no folder dialog ({})", SDL_GetError());
    return;
  }
  SDL_SetStringProperty(g_pick.props, SDL_PROP_FILE_DIALOG_TITLE_STRING, title.c_str());
  SDL_SetPointerProperty(g_pick.props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER,
                         window.native_handles().window);
  g_pick.open = true;
  SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFOLDER, FolderPicked, nullptr, g_pick.props);
}

// Take the parked answer, if there is one, and free what the dialog held. The
// properties outlive the call on purpose: the dialog reads them while it is up.
bool TakePickedFolder(int* target, base::String* path) {
  if (!g_pick.ready.load(std::memory_order_acquire))
    return false;
  *target = g_pick.target;
  *path = g_pick.path;
  if (g_pick.props != 0) {
    SDL_DestroyProperties(g_pick.props);
    g_pick.props = 0;
  }
  g_pick.path.clear();
  g_pick.open = false;
  g_pick.ready.store(false, std::memory_order_release);
  return true;
}

// Resolve a user-picked folder to a valid Data directory for `game`, or "" if
// neither the folder nor its Data subfolder holds that game's master plugin.
// Players sometimes pick the game root rather than its Data folder, so both are
// checked; the returned path is the one that actually contains the master.
base::String ResolvePickedDataDir(bethesda::Game game, const base::String& picked) {
  const auto& profile = bethesda::GameProfile::For(game);
  if (profile.base_masters.empty())
    return picked;  // unknown game: accept as-is
  const base::String master(profile.base_masters[0].c_str());
  std::error_code ec;
  if (fs::exists(fs::path(picked.c_str()) / master.c_str(), ec))
    return picked;
  const fs::path data = fs::path(picked.c_str()) / "Data";
  if (fs::exists(data / master.c_str(), ec))
    return data.string();
  return "";
}

}  // namespace

// Pull any persisted game paths and mods directory into the EngineConfig so a
// later launch resolves the same universes the wizard found (ResolveUniverses
// reads config_.extra_domains). Safe to call even when no setup.ini exists.
void LoadSetupConfig(Engine& engine) {
  Engine* const self = &engine;
  const base::Map<base::String, base::String> kv = ReadIni();
  if (kv.empty())
    return;
  for (const GameSpec& spec : kGameSpecs) {
    const auto it = kv.find(spec.key);
    if (it == kv.end() || it->second.empty())
      continue;
    ExtraDomainConfig d;
    d.game = spec.game;
    d.data_dir = it->second;
    d.plugins_txt = it->second + "/../plugins.txt";
    self->config_.known_games.push_back(d);
  }
  if (const auto it = kv.find("mods_dir"); it != kv.end() && !it->second.empty()) {
    self->config_.mods_dir = it->second;
    self->first_run_mods_dir_ = it->second;
  }
  // The name the wizard collected. Without this the setup wrote it and nothing
  // ever read it back, so the front screen kept showing the account name.
  // A --name on the command line was an explicit choice this run and wins.
  if (const auto it = kv.find("username");
      it != kv.end() && !it->second.empty() && self->config_.player_name.empty())
    self->config_.player_name = it->second;
}

// True once the wizard has been completed (setup.ini exists with done=1).
// RX_FORCE_FIRST_RUN forces the wizard back on for testing.
bool FirstRunComplete() {
  if (ForceFirstRun)
    return false;
  const auto kv = ReadIni();
  const auto it = kv.find("done");
  return it != kv.end() && it->second == "1";
}

void SetupFirstRun(Engine& engine) {
  Engine* const self = &engine;
  self->first_run_active_ = true;
  ResolveUniverses(engine);  // pre-detect installed games (config / env / Steam)
  if (self->first_run_mods_dir_.empty())
    self->first_run_mods_dir_ = DefaultModsDir();
  self->game_ui_.OpenFirstRun();
  self->debug_ui_.SetVisible(false);  // a clean front screen, no debug overlays
  RX_INFO("first-run setup wizard open");
}

namespace {

// Persist the wizard's choices and the done marker that suppresses it on later
// launches. data_dirs holds each column's located path ("" if not found); the
// privileged caller (Engine::UpdateFirstRun) gathers it from menu_universes_.
void WriteSetupIni(const base::Array<base::String, 3>& data_dirs,
                   const base::String& mods_dir,
                   const FirstRunRequest& r) {
  std::error_code ec;
  fs::create_directories(SetupDir(), ec);
  std::ofstream f(SetupFile(), std::ios::trunc);
  if (!f) {
    RX_WARN("first-run: could not write {}", SetupFile().string());
    return;
  }
  f << "done=1\n";
  for (int i = 0; i < 3; ++i)
    if (!data_dirs[i].empty())
      f << kGameSpecs[i].key << "=" << data_dirs[i].c_str() << "\n";
  f << "mods_dir=" << (mods_dir.empty() ? DefaultModsDir() : mods_dir).c_str() << "\n";
  f << "default_mode=" << r.mode << "\n";
  f << "difficulty=" << r.difficulty << "\n";
  f << "enable_mods=" << (r.enable_mods ? 1 : 0) << "\n";
  f << "share_diagnostics=" << (r.share_diagnostics ? 1 : 0) << "\n";
  f << "check_updates=" << (r.check_updates ? 1 : 0) << "\n";
  f << "rich_presence=" << (r.rich_presence ? 1 : 0) << "\n";
  // Omitted entirely when blank, so "no name chosen" and "chose an empty name"
  // stay the same thing and the account name keeps standing in.
  if (!r.username.empty())
    f << "username=" << r.username.c_str() << "\n";
  RX_INFO("first-run setup saved to {}", SetupFile().string());
}

}  // namespace

void Engine::UpdateFirstRun(f32 dt) {
  (void)dt;
  window_->SetRelativeMouseMode(false);  // free cursor so the wizard can be clicked

  // Validate a picked folder to game `idx`'s Data dir and mark it available.
  // Shared by the browse click and the auto-browse test hook below.
  auto accept_folder = [this](int idx, const base::String& picked) {
    if (idx < 0 || idx >= 3 || picked.empty())
      return;  // cancelled the dialog: nothing happened, say nothing
    MenuUniverse& u = menu_universes_[idx];
    const base::String data = ResolvePickedDataDir(u.game, picked);
    if (data.empty()) {
      // The pick did not hold the game. Name the file that was looked for, so
      // the player can tell "wrong folder" from "wrong game" without a log.
      const auto& profile = bethesda::GameProfile::For(u.game);
      const base::String master =
          profile.base_masters.empty() ? base::String("its master file")
                                       : base::String(profile.base_masters[0].c_str());
      first_run_notice_ = "No " + master + " in that folder. Pick the Data folder of your " +
                          u.name + " install, or the install itself.";
      RX_WARN("first-run: {} not found under {}", u.name, picked);
      return;
    }
    u.data_dir = data;
    u.plugins_txt = data + "/../plugins.txt";
    u.available = true;
    first_run_notice_.clear();
    RX_INFO("first-run: located {} at {}", u.name, data);
  };

  // A folder the dialog has finished with, from whenever the player closed it.
  {
    int target = 0;
    base::String picked;
    if (TakePickedFolder(&target, &picked)) {
      if (target == kPickModsDir) {
        if (!picked.empty())
          first_run_mods_dir_ = picked;
      } else {
        accept_folder(target, picked);
      }
    }
  }

  // Test hook: RX_FIRSTRUN_AUTOBROWSE=<0..2> browses that column once on the
  // first frame (mirrors RX_MENU_AUTOPLAY), so the validation and locate path
  // run without a mouse. Pair with RX_PICK_OVERRIDE for the folder, which is
  // taken here rather than through the dialog so the whole flow still resolves
  // within the frame it fired on.
  if (const char* ab = FirstrunAutobrowse.get()) {
    static bool fired = false;
    if (!fired) {
      fired = true;
      if (const char* o = PickOverride.get())
        accept_folder(std::atoi(ab), o);
      else
        PickFolder(*window_, std::atoi(ab), "auto");
    }
  }

  // Keyboard conveniences: Accept advances the page (the primary button), Cancel
  // steps back / cancels at the first page. The mouse drives everything else.
  if (actions_->pressed(Action::kMenuAccept))
    game_ui_.FirstRunNext();
  if (actions_->pressed(Action::kMenuCancel))
    game_ui_.FirstRunBack();

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
  view.notice = first_run_notice_;
  game_ui_.SetFirstRunView(view);

  // Test hook: RX_FIRSTRUN_AUTOLAUNCH advances one page per frame to the end and
  // launches, so the setup->main-menu handoff can be verified headlessly. Runs
  // after the view push so the locate-page gate sees any auto-browsed game.
  if (FirstrunAutolaunch)
    game_ui_.FirstRunNext();

  const FirstRunRequest req = game_ui_.PollFirstRunRequest();
  switch (req.kind) {
    case FirstRunRequest::Kind::kBrowseGame: {
      if (req.index < 0 || req.index >= 3)
        break;
      PickFolder(*window_, req.index,
                 "Locate the " + menu_universes_[req.index].name + " Data folder");
      break;
    }
    case FirstRunRequest::Kind::kBrowseMods:
      PickFolder(*window_, kPickModsDir, "Choose the Recreation mods directory");
      break;
    case FirstRunRequest::Kind::kLaunch: {
      // Record the located universes so the SetupMainMenu that follows resolves
      // to the same paths, and gather them for the ini. known_games, not
      // extra_domains: these are the games the player owns, not games to mount
      // all at once (see EngineConfig).
      base::Array<base::String, 3> data_dirs;
      for (int i = 0; i < 3; ++i) {
        const MenuUniverse& u = menu_universes_[i];
        if (!u.available || u.data_dir.empty())
          continue;
        data_dirs[i] = u.data_dir;
        ExtraDomainConfig d;
        d.game = kGameSpecs[i].game;
        d.data_dir = u.data_dir;
        d.plugins_txt = u.plugins_txt.empty() ? (u.data_dir + "/../plugins.txt") : u.plugins_txt;
        config_.known_games.push_back(d);
      }
      config_.mods_dir = first_run_mods_dir_.empty() ? DefaultModsDir() : first_run_mods_dir_;
      // Apply the name now, not just persist it: the front screen this hands off
      // to reads config_.player_name, and waiting for a restart to show the name
      // someone just typed is the kind of thing that reads as "it did not work".
      if (!req.username.empty())
        config_.player_name = req.username;
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

}  // namespace rx
