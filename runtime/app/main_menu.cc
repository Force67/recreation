#include <base/algorithm.h>
#include <base/containers/array.h>
#include <base/containers/pair.h>
#include <base/containers/vector.h>
#include <base/memory/move.h>
#include <base/option.h>
#include <base/strings/to_string.h>
#include <base/strings/xstring.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "runtime/app/engine.h"

#if !defined(_WIN32)
#include <pwd.h>     // getpwuid for the local profile name
#include <unistd.h>  // gethostname / getuid
#endif

#ifndef RECREATION_VERSION
#define RECREATION_VERSION "0.1.0"
#endif

#include <algorithm>  // std::max / std::clamp in the procedural backdrop painter
#include <cmath>      // std::floor / std::sqrt / std::fabs for the scene noise
#include <utility>    // base::move for the procedural meshes

#include "asset/mesh.h"
#include "components/script/games/skyrim/skyrim_bindings.h"
#include "core/log.h"
#include "runtime/app/engine_internal.h"
#include "runtime/ui/thumbnailer.h"  // off-screen clay render of the hero centerpiece

#if defined(RECREATION_HAS_UGUI)
#include <stb_image.h>  // the key-art PNGs the launch tiles are painted from
#endif

// Where the base games' key art ships (runtime/ui/art, beside the .ugui
// screens). Baked in absolute by CMake so a dev build finds the source tree.
#ifndef RECREATION_UI_ART_DIR_DEFAULT
#define RECREATION_UI_ART_DIR_DEFAULT "runtime/ui/art"
#endif

// The NEXUS main menu: the front door a bare windowed launch opens. Resolves the
// installed universes (Steam/env scan), drives menu navigation and the
// select-then-PLAY request flow, loads a universe on demand, and refreshes the
// front-screen identity/stats plus the cached scene backdrops.
namespace rx {

// Config toggles formerly read from getenv (populated by base::InitOptionsFromEnv).
static base::Option<bool> HideDebugUi{"hide.debug.ui", false, "RX_HIDE_DEBUG_UI"};
static base::Option<bool> MenuCapture{"menu.capture", false, "RX_MENU_CAPTURE"};
static base::Option<const char*> MenuAutoplay{"menu.autoplay", nullptr, "RX_MENU_AUTOPLAY"};
// One more Steam install directory to search, for a layout the standard
// locations miss (a second drive's copy, a portable install, a test rig).
static base::Option<const char*> SteamRoot{"steam.root", nullptr, "RX_STEAM_ROOT"};

// Gold001, the currency record a Skyrim inventory counts money in.
static constexpr u32 kGoldFormId = 0x0000000f;

// The C# gameplay modules each universe installs once it is the primary domain,
// previewed on the menu's Mods screen before the game loads. Skyrim and
// Starfield ship a full layer (SkyrimMod / StarfieldMod); Fallout 4 runs the
// shared SDK for now.
static base::Vector<base::String> MenuModulesFor(int universe) {
  switch (universe) {
    case 0:
      return {"Attribute Regeneration",
              "Quest Progress",
              "Combat Tracker",
              "Essential Protection",
              "Injury Slowdown",
              "Time of Day",
              "Encumbrance",
              "Location Discovery",
              "Harvesting",
              "Book Learning",
              "Racial Abilities",
              "Blessing Upkeep",
              "Vampirism",
              "Lycanthropy",
              "Shout Cooldown"};
    case 2:
      return {"Oxygen / CO2",  "Environmental Hazards", "Mass Encumbrance", "Well Rested",
              "Quest Rewards", "Combat Rewards",        "Discovery XP",     "Notifications"};
    default:
      return {"Recreation SDK", "Event Bus", "Fallout 4 content domain"};
  }
}

namespace {

// One staged gamemode assembly, as described by the JSON manifest sitting beside
// it in <managed dir>/gamemodes. The menu runs before the .NET runtime boots, so
// a mode cannot be asked what it is through managed reflection: the manifest is
// the only description available here.
struct GameModeManifest {
  base::String assembly;  // file stem, so an unmanifested .dll can be reported
  base::String id;        // arms this mode once the domain is up
  base::String kind;      // "mode" (its own tile) or "ruleset" (a game's base layer)
  base::String title;
  base::String detail;
  base::String domain;  // menu universe the mode runs in ("Skyrim", ...)
  base::String art;     // key-art PNG, resolved to a full path; empty if none
};

mem_size JsonSkipSpace(const base::String& text, mem_size i) {
  while (i < text.size() &&
         (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n'))
    ++i;
  return i;
}

// Reads the quoted string at text[i] (i on the opening quote) into out, leaving
// i past the closing quote. Manifest fields are plain UTF-8, so a \u escape is a
// parse failure rather than something to half-decode.
bool JsonString(const base::String& text, mem_size& i, base::String& out) {
  if (i >= text.size() || text[i] != '"')
    return false;
  out.clear();
  for (++i; i < text.size(); ++i) {
    if (text[i] == '"') {
      ++i;
      return true;
    }
    if (text[i] != '\\') {
      out.push_back(text[i]);
      continue;
    }
    if (++i >= text.size())
      break;
    switch (text[i]) {
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case 'u':
        return false;
      default:
        out.push_back(text[i]);
        break;
    }
  }
  return false;
}

// Reads a flat JSON object of string fields. Purpose-built for the manifest
// rather than a JSON dependency: five strings do not justify one. A value that
// is not a string is skipped, so a field a future manifest adds cannot fail the
// parse; nesting is not supported and is what the "flat" in the name means.
bool ParseFlatJson(const base::String& text,
                   base::Vector<base::Pair<base::String, base::String>>& out) {
  mem_size i = JsonSkipSpace(text, 0);
  if (i >= text.size() || text[i] != '{')
    return false;
  ++i;
  for (;;) {
    i = JsonSkipSpace(text, i);
    if (i >= text.size())
      return false;
    if (text[i] == '}')
      return true;
    if (text[i] == ',') {
      ++i;
      continue;
    }
    base::String key;
    if (!JsonString(text, i, key))
      return false;
    i = JsonSkipSpace(text, i);
    if (i >= text.size() || text[i] != ':')
      return false;
    i = JsonSkipSpace(text, i + 1);
    if (i >= text.size())
      return false;
    if (text[i] == '"') {
      base::String value;
      if (!JsonString(text, i, value))
        return false;
      out.push_back({key, value});
      continue;
    }
    while (i < text.size() && text[i] != ',' && text[i] != '}')
      ++i;
  }
}

// Fields the menu understands; anything else in the file is ignored. A manifest
// without an id describes nothing that can be launched, so it fails.
bool ReadManifest(const base::String& path, GameModeManifest& out) {
  std::ifstream f(path.c_str());
  if (!f)
    return false;
  base::String text;
  std::string source;
  while (std::getline(f, source)) {
    text.append(source.c_str(), source.size());
    text.push_back('\n');
  }
  base::Vector<base::Pair<base::String, base::String>> fields;
  if (!ParseFlatJson(text, fields))
    return false;
  for (const auto& [key, value] : fields) {
    if (key == "id")
      out.id = value;
    else if (key == "kind")
      out.kind = value;
    else if (key == "title")
      out.title = value;
    else if (key == "detail")
      out.detail = value;
    else if (key == "domain")
      out.domain = value;
    else if (key == "art")
      out.art = value;
  }
  return !out.id.empty();
}

// Collects the manifests staged in <managed dir>/gamemodes, sorted by id so the
// grid does not reshuffle with directory order. An assembly with no manifest is
// reported rather than quietly dropped: a mistyped manifest name would otherwise
// just make the mode vanish from the menu.
base::Vector<GameModeManifest> ScanGameModes() {
  base::Vector<GameModeManifest> out;
  const char* managed = std::getenv("RECREATION_SCRIPTING_DIR");
  if (!managed || !*managed)
    return out;
  namespace fs = std::filesystem;
  const fs::path root = fs::path(managed) / "gamemodes";
  std::error_code ec;
  if (!fs::exists(root, ec))
    return out;
  base::Vector<base::String> assemblies;
  for (const fs::directory_entry& entry : fs::directory_iterator(root, ec)) {
    const fs::path& p = entry.path();
    if (p.extension() == ".dll") {
      assemblies.push_back(p.stem().string());
      continue;
    }
    if (p.extension() != ".json")
      continue;
    GameModeManifest m;
    m.assembly = p.stem().string();
    if (!ReadManifest(p.string(), m)) {
      RX_WARN("gamemodes: {} is not a readable manifest", p.string());
      continue;
    }
    if (!m.art.empty()) {
      const fs::path art = root / m.art.c_str();
      if (fs::exists(art, ec)) {
        m.art = art.string();
      } else {
        RX_WARN("gamemodes: {} names key art {} that is not there", m.id, m.art);
        m.art.clear();
      }
    }
    out.push_back(m);
  }
  for (const base::String& assembly : assemblies) {
    bool described = false;
    for (const GameModeManifest& m : out)
      described = described || m.assembly == assembly;
    if (!described)
      RX_WARN("gamemodes: {}.dll has no manifest beside it, not listed", assembly);
  }
  base::Sort(out.data(), out.data() + out.size(),
             [](const GameModeManifest& a, const GameModeManifest& b) {
               return a.id.compare(b.id) < 0;
             });
  return out;
}

// Load-order length from a game's plugins.txt: non-blank, non-comment lines.
// Bethesda's launchers mark an enabled plugin with a leading '*'; both forms
// count, the file is the spine either way. 0 when there is no plugins.txt, and
// the tile then omits the count rather than guessing one.
int CountPlugins(const base::String& path) {
  std::ifstream f(path.c_str());
  if (!f)
    return 0;
  int count = 0;
  std::string source;
  while (std::getline(f, source)) {
    const base::String line(source.c_str(), source.size());
    const mem_size first = line.find_first_not_of(" \t\r\n");
    if (first == base::String::npos || line[first] == '#')
      continue;
    ++count;
  }
  return count;
}

// Where a session parks a clean frame of this game's world (TickMenuCapture
// writes it, RX_MENU_CAPTURE arms it).
base::String WorldCapturePath(bethesda::Game game) {
  return "thumbs/menu_" + GameSlug(game) + ".png";
}

// A base game tile's key art: a capture of its own world if a past session left
// one, else the PNG shipped in the repo. Empty when neither is there, which is
// what puts the tile on the painted fallback.
base::String GameKeyArt(bethesda::Game game) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const base::String live = WorldCapturePath(game);
  if (fs::exists(live.c_str(), ec))
    return live;
  const base::String shipped =
      base::String(RECREATION_UI_ART_DIR_DEFAULT "/menu_") + GameSlug(game) + ".png";
  if (fs::exists(shipped.c_str(), ec))
    return shipped;
  return {};
}

// Every Steam library on this machine, as ".../steamapps/common" directories.
// Games live wherever the player put them, so guessing paths only ever works on
// the machine the guess was written on: the list comes from Steam's own
// libraryfolders.vdf, which records each library root the player has added,
// including ones on other drives. The install locations below exist only to
// find that file. RX_STEAM_ROOT names one more, for an install none of the
// standard locations covers.
base::Vector<base::String> SteamCommonRoots() {
  namespace fs = std::filesystem;
  base::Vector<base::String> roots;
  std::error_code ec;
  auto add = [&](const fs::path& p) {
    if (p.empty() || !fs::exists(p, ec))
      return;
    base::String s = p.string().c_str();
    for (const base::String& have : roots)
      if (have == s)
        return;
    roots.push_back(base::move(s));
  };

  base::Vector<fs::path> installs;
  if (const char* extra = SteamRoot.get(); extra != nullptr && *extra)
    installs.push_back(fs::path(extra));
#if defined(_WIN32)
  for (const char* var : {"ProgramFiles(x86)", "ProgramFiles"})
    if (const char* pf = std::getenv(var))
      installs.push_back(fs::path(pf) / "Steam");
#elif defined(__APPLE__)
  if (const char* home = std::getenv("HOME"))
    installs.push_back(fs::path(home) / "Library" / "Application Support" / "Steam");
#else
  if (const char* home = std::getenv("HOME")) {
    installs.push_back(fs::path(home) / ".local" / "share" / "Steam");
    installs.push_back(fs::path(home) / ".steam" / "steam");
    installs.push_back(fs::path(home) / ".steam" / "root");
    installs.push_back(fs::path(home) / ".var" / "app" / "com.valvesoftware.Steam" / "data" /
                       "Steam");  // flatpak
  }
#endif

  for (const fs::path& install : installs) {
    add(install / "steamapps" / "common");
    std::ifstream vdf((install / "steamapps" / "libraryfolders.vdf").string().c_str());
    if (!vdf)
      continue;
    for (std::string line; std::getline(vdf, line);) {
      const size_t key = line.find("\"path\"");
      if (key == std::string::npos)
        continue;
      const size_t open = line.find('"', key + 6);
      if (open == std::string::npos)
        continue;
      const size_t close = line.find('"', open + 1);
      if (close == std::string::npos)
        continue;
      std::string path = line.substr(open + 1, close - open - 1);
      // The only escape a library path carries is the doubled Windows separator.
      for (size_t p = path.find("\\\\"); p != std::string::npos; p = path.find("\\\\", p + 1))
        path.erase(p, 1);
      add(fs::path(path) / "steamapps" / "common");
    }
  }
  return roots;
}

}  // namespace

void ResolveUniverses(Engine& engine) {
  Engine* const self = &engine;
  namespace fs = std::filesystem;
  struct Spec {
    bethesda::Game game;
    const char* name;
    const char* env;
    const char* subdir;
  };
  const Spec specs[3] = {
      {bethesda::Game::kSkyrimSe, "Skyrim", "RX_SKYRIM_DATA", "Skyrim Special Edition/Data"},
      {bethesda::Game::kFallout4, "Fallout 4", "RX_FALLOUT4_DATA", "Fallout 4/Data"},
      {bethesda::Game::kStarfield, "Starfield", "RX_STARFIELD_DATA", "Starfield/Data"},
  };
  // Where to look when no explicit path is configured.
  const base::Vector<base::String> roots = SteamCommonRoots();
  RX_INFO("steam libraries: {}", roots.size());
  auto from_config = [&](bethesda::Game g) -> base::Pair<base::String, base::String> {
    if (self->config_.game == g && !self->config_.data_dir.empty())
      return {self->config_.data_dir, self->config_.plugins_txt};
    for (const auto& d : self->config_.extra_domains)
      if (d.game == g && !d.data_dir.empty())
        return {d.data_dir, d.plugins_txt};
    return {"", ""};
  };
  for (int i = 0; i < 3; ++i) {
    Engine::MenuUniverse& u = self->menu_universes_[i];
    u.game = specs[i].game;
    u.name = specs[i].name;
    u.data_dir.clear();
    u.plugins_txt.clear();
    auto [cd, cp] = from_config(specs[i].game);  // explicit --data-dir / --add-game wins
    if (!cd.empty()) {
      u.data_dir = cd;
      u.plugins_txt = cp;
    }
    if (u.data_dir.empty())
      if (const char* e = std::getenv(specs[i].env))
        u.data_dir = e;        // env override
    if (u.data_dir.empty()) {  // Steam scan
      for (const base::String& root : roots) {
        std::error_code ec;
        fs::path p = fs::path(root.c_str()) / specs[i].subdir;
        if (fs::exists(p.c_str(), ec)) {
          u.data_dir = p.string();
          break;
        }
      }
    }
    if (u.plugins_txt.empty() && !u.data_dir.empty())
      u.plugins_txt = u.data_dir + "/../plugins.txt";
    std::error_code ec;
    u.available = !u.data_dir.empty() && fs::exists(u.data_dir.c_str(), ec);
    RX_INFO("menu universe {}: {} -> {}", i, u.name.c_str(),
            u.available ? u.data_dir.c_str() : "(unavailable)");
  }
}

// The launch grid: the three universes, then every mounted mode under the game
// it runs in. Flat, so a mode is picked exactly the way a game is. Requires
// ResolveUniverses to have run.
void BuildMenuEntries(Engine& engine) {
  Engine* const self = &engine;
  self->menu_entries_.clear();
  self->menu_entry_art_.clear();
  self->menu_mode_ids_.clear();
  for (int i = 0; i < static_cast<int>(self->menu_universes_.size()); ++i) {
    const Engine::MenuUniverse& u = self->menu_universes_[i];
    GameUi::MenuEntry e;
    e.kind = GameUi::MenuEntry::Kind::kGame;
    e.title = u.name;
    e.state = u.available ? "Ready" : "Not located";
    e.universe = i;
    e.plugins = u.available ? CountPlugins(u.plugins_txt) : 0;
    e.available = u.available;
    // The cell count only exists once the game's records are loaded, which the
    // menu deliberately has not done, so the detail line is the load order alone.
    if (e.plugins > 0)
      e.detail = base::ToString(e.plugins) + " plugins";
    self->menu_entries_.push_back(e);
    self->menu_entry_art_.push_back(GameKeyArt(u.game));
  }
  int mounted = 0;
  for (const GameModeManifest& m : ScanGameModes()) {
    if (m.kind != "mode")  // a per-game base ruleset, not something to pick
      continue;
    // Every mode is optional content the managed side must leave dormant unless
    // it was armed, including one whose domain resolves to no tile below.
    self->menu_mode_ids_.push_back(m.id);
    int universe = -1;
    for (int i = 0; i < static_cast<int>(self->menu_universes_.size()); ++i)
      if (self->menu_universes_[i].name == m.domain)
        universe = i;
    if (universe < 0) {
      RX_WARN("gamemodes: {} runs in unknown domain {}", m.id, m.domain);
      continue;
    }
    GameUi::MenuEntry e;
    e.kind = GameUi::MenuEntry::Kind::kMode;
    e.title = m.title.empty() ? m.id : m.title;
    e.detail = m.detail;
    e.domain = m.domain;
    e.state = "Mounted";
    e.mode_id = m.id;
    e.universe = universe;
    e.available = self->menu_universes_[universe].available;
    self->menu_entries_.push_back(e);
    self->menu_entry_art_.push_back(m.art);
    ++mounted;
  }
  self->game_ui_.SetMainMenuEntries(self->menu_entries_);
  RX_INFO("menu: {} launch entries ({} game modes mounted)", self->menu_entries_.size(), mounted);
}

void SetupMainMenu(Engine& engine) {
  Engine* const self = &engine;
  self->main_menu_active_ = true;
  ResolveUniverses(engine);
  base::Vector<base::String> names;
  base::Vector<bool> avail;
  for (const Engine::MenuUniverse& u : self->menu_universes_) {
    names.push_back(u.name);
    avail.push_back(u.available);
  }
  self->game_ui_.SetMainMenuUniverses(names, avail);
  BuildMenuEntries(engine);  // the grid GenerateMenuBackdrops then paints art for
  self->game_ui_.OpenMainMenu();
  self->game_ui_.SetMainMenuNews({{"Welcome to Recreation", "v" RECREATION_VERSION}});
  self->GenerateMenuBackdrops();      // original procedural concept art per universe
  self->debug_ui_.SetVisible(false);  // a clean front screen, no debug overlays
  RX_INFO("nexus main menu open");
}

void EnterUniverse(Engine& engine,
                   int idx,
                   bool multiplayer,
                   bool host,
                   const base::String& join_address) {
  Engine* const self = &engine;
  if (idx < 0 || idx >= static_cast<int>(self->menu_universes_.size()))
    return;
  const Engine::MenuUniverse& u = self->menu_universes_[idx];
  if (!u.available) {
    RX_WARN("universe {} has no data; cannot enter", u.name);
    return;
  }
  RX_INFO("entering universe {}{}", u.name,
          multiplayer ? (host ? " (hosting)" : " (joining)") : "");
  self->config_.game = u.game;
  self->config_.data_dir = u.data_dir;
  self->config_.plugins_txt = u.plugins_txt;
  self->config_.spawn_player = true;
  if (multiplayer) {
    if (host)
      self->config_.host_server = true;
    else
      self->config_.connect_address = join_address;
  }
  // A mode arms on top of the domain's base ruleset, it does not replace it, so
  // the pick only adds an id. BootManagedScripting (inside LoadGameData) passes
  // it, and the full set of modes, through the managed handshake.
  if (!self->menu_mode_id_.empty())
    RX_INFO("arming game mode {} on {}", self->menu_mode_id_, u.name);
  self->game_ui_.CloseMainMenu();
  self->main_menu_active_ = false;
  self->debug_ui_.SetVisible(!HideDebugUi);
  if (!LoadGameData(engine)) {  // boots the managed world, so the game's C# module installs
    RX_ERROR("failed to load universe {}", u.name);
    return;
  }
  // Opt-in (RX_MENU_CAPTURE): grab a clean frame of this world for the menu
  // backdrop cache once it has streamed in, so a later menu shows the real scene.
  // Off by default, since a mid-stream grab can catch an unsettled frame.
  if (!self->config_.headless && MenuCapture) {
    self->menu_capture_path_ = WorldCapturePath(self->config_.game);
    self->menu_capture_countdown_ = 600;  // ~10s at 60fps to let streaming + RT settle
  }
#if RECREATION_HAS_NET
  if (self->config_.host_server || !self->config_.connect_address.empty()) {
    if (!StartNetworking(engine))
      RX_WARN("networking failed to start");
  }
#endif
}

void Engine::UpdateMainMenu(f32 dt) {
  (void)dt;
  const InputState& in = window_->input();
  window_->SetRelativeMouseMode(false);  // free cursor so the menu can be clicked

  // Test hook: RX_MENU_AUTOPLAY=<0|1|2> drives the same select-then-PLAY path a
  // mouse/keyboard would, so the menu->request->boot chain runs without input.
  if (const char* ap = MenuAutoplay.get()) {
    static int beat = 0;
    if (++beat == 45) {
      game_ui_.MainMenuMove(std::atoi(ap) - game_ui_.selected_universe(), 0);  // pick the column
      game_ui_.MainMenuActivate();  // PLAY -> kEnterUniverse, dispatched by the poll below
    }
  }

  // Menu actions (keyboard arrows + gamepad dpad/stick + South/East) drive the
  // NEXUS menu; WASD and Space are kept as extra keyboard conveniences. Routed
  // through the same helpers so mouse, keyboard, and pad share one selection.
  if (actions_->pressed(Action::kMenuUp) || in.key_pressed(Key::kW))
    game_ui_.MainMenuMove(0, -1);
  if (actions_->pressed(Action::kMenuDown) || in.key_pressed(Key::kS))
    game_ui_.MainMenuMove(0, +1);
  if (actions_->pressed(Action::kMenuLeft) || in.key_pressed(Key::kA))
    game_ui_.MainMenuMove(-1, 0);
  if (actions_->pressed(Action::kMenuRight) || in.key_pressed(Key::kD))
    game_ui_.MainMenuMove(+1, 0);
  // A page is exactly two tile rows, so paging reuses the grid move rather than
  // needing its own cursor. The footer advertises Q/E, so it has to work.
  if (in.key_pressed(Key::kQ))
    game_ui_.MainMenuMove(0, -2);
  if (in.key_pressed(Key::kE))
    game_ui_.MainMenuMove(0, +2);
  if (actions_->pressed(Action::kMenuAccept) || in.key_pressed(Key::kSpace))
    game_ui_.MainMenuActivate();
  if (actions_->pressed(Action::kMenuCancel))
    game_ui_.MainMenuBack();

  // No RefreshMenuData here: the render path runs it every frame, menu or not,
  // because the HUD reads the same block.
  const MainMenuRequest req = game_ui_.PollMainMenuRequest();
  switch (req.kind) {
    case MainMenuRequest::Kind::kEnterUniverse:
      menu_mode_id_ = req.mode_id;  // empty for a base game tile
      EnterUniverse(*this, req.universe, false, false, "");
      break;
    case MainMenuRequest::Kind::kHostServer:
      EnterUniverse(*this, req.universe, true, true, "");
      break;
    case MainMenuRequest::Kind::kJoinServer:
      EnterUniverse(*this, req.universe, true, false,
                    req.address.empty() ? base::String("127.0.0.1") : req.address);
      break;
    case MainMenuRequest::Kind::kQuit:
      RequestQuit();
      break;
    case MainMenuRequest::Kind::kOpenUrl:
      if (!req.url.empty()) {
#if defined(_WIN32)
        const base::String cmd = "start \"\" \"" + req.url + "\"";
#elif defined(__APPLE__)
        const base::String cmd = "open \"" + req.url + "\" >/dev/null 2>&1 &";
#else
        const base::String cmd = "xdg-open \"" + req.url + "\" >/dev/null 2>&1 &";
#endif
        if (std::system(cmd.c_str()) != 0)
          RX_WARN("could not open url {}", req.url);
        else
          RX_INFO("opened url {}", req.url);
      }
      break;
    case MainMenuRequest::Kind::kNone:
      break;
  }
}

void Engine::RefreshMenuData() {
  MainMenuStats stats;

  // Real local-profile identity for the front screen: the OS account and host,
  // plus the configured multiplayer handle (falls back to the login name).
  // Resolved once: this runs every frame now, and the machine's name does not
  // change under it.
  static const base::Pair<base::String, base::String> identity = [] {
    base::String login;
    char host[256] = {0};
#if !defined(_WIN32)
    if (const struct passwd* pw = getpwuid(getuid()); pw && pw->pw_name)
      login = pw->pw_name;
    if (gethostname(host, sizeof(host) - 1) != 0)
      host[0] = '\0';
#endif
    if (login.empty()) {
      if (const char* u = std::getenv("USER"))
        login = u;
      else if (const char* u2 = std::getenv("USERNAME"))
        login = u2;
    }
    if (!host[0]) {
      if (const char* h = std::getenv("HOSTNAME"))
        std::snprintf(host, sizeof(host), "%s", h);
      else if (const char* h2 = std::getenv("COMPUTERNAME"))
        std::snprintf(host, sizeof(host), "%s", h2);
    }
    return base::Pair<base::String, base::String>{
        login.empty() ? base::String("local") : login,
        host[0] ? base::String(host) : base::String("this machine")};
  }();

  const base::String handle(config_.player_name.c_str());
  stats.account = identity.first;
  stats.machine = identity.second;
  stats.build = RECREATION_VERSION;
  stats.player_name = (handle.empty() || handle == "player") ? stats.account : handle;
  stats.game_days = clock_ ? clock_->game_days() : 0.0;
  stats.net_status = "Offline";

  // The live character, once a universe is up. Read out of the bindings, which
  // is the same store a loaded savegame writes into, so the banner and the HUD
  // gold counter say what the running game believes rather than a placeholder.
  // `location` stays empty: nothing in the engine names the cell the player is
  // standing in yet.
  stats.in_game = !main_menu_active_ && game_ != bethesda::Game::kUnknown;
  if (stats.in_game) {
    for (const MenuUniverse& u : menu_universes_) {
      if (u.game == game_)
        stats.universe = u.name;
    }
  }
  if (stats.in_game && script_bindings_) {
    const script::papyrus::ObjectRef player{
        bethesda::GlobalFormId{0, bethesda::kPlayerFormId}.packed()};
    auto& b = *script_bindings_;
    stats.level = b.GetLevel(player);
    stats.gold = b.GetItemCount(
        player, script::papyrus::ObjectRef{bethesda::GlobalFormId{0, kGoldFormId}.packed()});
    stats.health = b.GetActorValuePercentage(player, "Health");
    stats.magicka = b.GetActorValuePercentage(player, "Magicka");
    stats.stamina = b.GetActorValuePercentage(player, "Stamina");
    stats.active_quests = static_cast<int>(b.quest_system().RunningCount());
  }

  int avail = 0;
  for (const auto& u : menu_universes_)
    if (u.available)
      ++avail;
  stats.universes_available = avail;

#if RECREATION_HAS_NET
  if (server_session_) {
    stats.players_online = static_cast<int>(server_session_->client_count());
    stats.net_status = "Hosting :" + base::ToString(config_.port);
  } else if (client_session_) {
    stats.net_status = client_session_->joined() ? "Connected" : "Connecting";
  }
#endif
  game_ui_.SetMainMenuStats(stats);
  // A mode tile shows the modules of the game it runs in, so the Mods screen
  // follows the grid selection rather than the universe column.
  const int selected = game_ui_.selected_entry();
  const int universe = (selected >= 0 && selected < static_cast<int>(menu_entries_.size()))
                           ? menu_entries_[selected].universe
                           : game_ui_.selected_universe();
  game_ui_.SetMainMenuMods(MenuModulesFor(universe));
}

// ---------------------------------------------------------------------------
// Procedural menu concept art. Each universe pane is painted per pixel from an
// atmospheric sky, a few silhouette layers, and a grain/vignette pass, so the
// front screen ships as original, self-contained art with no external images.
// The looks evoke each game without copying any asset: cold overcast peaks
// (Skyrim), a warm dawn over a ruined city with a lone wanderer and companion
// (Fallout 4), and a banded planet over a dark plain with a landed ship
// (Starfield).
// ---------------------------------------------------------------------------
namespace {

struct Rgb {
  float r, g, b;
};

inline float Clamp01(float v) {
  return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}
inline float Mixf(float a, float b, float t) {
  return a + (b - a) * t;
}
inline Rgb Mix(Rgb a, Rgb b, float t) {
  return {Mixf(a.r, b.r, t), Mixf(a.g, b.g, t), Mixf(a.b, b.b, t)};
}
inline Rgb Add(Rgb a, Rgb b, float s) {
  return {a.r + b.r * s, a.g + b.g * s, a.b + b.b * s};
}
inline float Smooth(float e0, float e1, float x) {
  const float t = Clamp01((x - e0) / (e1 - e0));
  return t * t * (3.f - 2.f * t);
}

// Deterministic value hash in [0,1), the only randomness the painter uses.
inline float Hash2(int x, int y, int seed) {
  u32 h = static_cast<u32>(x) * 374761393u + static_cast<u32>(y) * 668265263u +
          static_cast<u32>(seed) * 2654435761u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return static_cast<float>((h ^ (h >> 16)) & 0xffffffu) / static_cast<float>(0xffffffu);
}
inline float ValueNoise(float x, float y, int seed) {
  const int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y));
  const float xf = x - xi, yf = y - yi;
  const float u = xf * xf * (3.f - 2.f * xf), v = yf * yf * (3.f - 2.f * yf);
  const float a = Hash2(xi, yi, seed), b = Hash2(xi + 1, yi, seed);
  const float c = Hash2(xi, yi + 1, seed), d = Hash2(xi + 1, yi + 1, seed);
  return Mixf(Mixf(a, b, u), Mixf(c, d, u), v);
}
inline float Fbm(float x, float y, int seed, int octaves) {
  float sum = 0.f, amp = 0.5f, norm = 0.f;
  for (int i = 0; i < octaves; ++i) {
    sum += amp * ValueNoise(x, y, seed + i * 31);
    norm += amp;
    x *= 2.f;
    y *= 2.f;
    amp *= 0.5f;
  }
  return sum / norm;
}
// Soft filled-circle coverage in pixel space (1 inside, 0 outside).
inline float Disc(float px, float py, float cx, float cy, float r, float soft) {
  const float d = std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
  return 1.f - Smooth(r - soft, r + soft, d);
}

// Paint one universe pane into a fresh RGBA8 buffer (straight alpha, fully
// opaque). universe: 0 Skyrim, 1 Fallout 4, 2 Starfield.
base::Vector<unsigned char> PaintBackdrop(int universe, int W, int H) {
  base::Vector<unsigned char> out(static_cast<size_t>(W) * H * 4);
  const float fw = static_cast<float>(W), fh = static_cast<float>(H);
  for (int y = 0; y < H; ++y) {
    const float fy = static_cast<float>(y);
    const float v = fy / (fh - 1.f);  // 0 top .. 1 bottom
    for (int x = 0; x < W; ++x) {
      const float fx = static_cast<float>(x);
      const float u = fx / (fw - 1.f);
      Rgb c{0.f, 0.f, 0.f};

      // One cohesive dark field shared by all three panes; only a soft,
      // universe-tinted glow behind the hero object differs, so the panes read as
      // a single continuous atmosphere rather than three coloured boxes.
      const Rgb base0{0.022f, 0.029f, 0.044f}, base1{0.044f, 0.056f, 0.080f};
      c = Mix(base0, base1, Smooth(0.0f, 1.0f, v));
      // One soft cool glow, identical in every pane (no warm/brown tint), centred
      // so it sits behind the hero gem.
      c = Add(c, Rgb{0.27f, 0.37f, 0.53f},
              Disc(fx, fy, fw * 0.5f, fh * 0.42f, fh * 0.46f, fh * 0.46f) * 0.22f);
      c = Add(c, Rgb{1.f, 1.f, 1.f}, (Fbm(u * 3.f, v * 4.f, 11, 3) - 0.5f) * 0.02f);  // faint haze
      if (universe == 2 && Hash2(x, y, 123) > 0.992f)
        c = Add(c, Rgb{0.9f, 0.95f, 1.0f}, Hash2(x, y, 7) * 0.7f + 0.2f);  // stars

      // shared finishing: film grain + edge vignette
      const float g = (Hash2(x, y, 9090) - 0.5f) * 0.035f;
      c = Add(c, Rgb{1.f, 1.f, 1.f}, g);
      const float r = std::sqrt((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f)) * 1.7f;
      const float vig = 1.f - 0.28f * Smooth(0.55f, 1.30f, r);
      c.r *= vig;
      c.g *= vig;
      c.b *= vig;

      const size_t o = (static_cast<size_t>(y) * W + x) * 4;
      out[o + 0] = static_cast<unsigned char>(Clamp01(c.r) * 255.f + 0.5f);
      out[o + 1] = static_cast<unsigned char>(Clamp01(c.g) * 255.f + 0.5f);
      out[o + 2] = static_cast<unsigned char>(Clamp01(c.b) * 255.f + 0.5f);
      out[o + 3] = 255;
    }
  }
  return out;
}

// A stable seed for one launch entry, so its painted art looks the same every
// launch and two entries never collide by position in the grid.
inline int TitleSeed(const base::String& title) {
  u32 h = 2166136261u;
  for (char c : title)
    h = (h ^ static_cast<u32>(static_cast<unsigned char>(c))) * 16777619u;
  return static_cast<int>(h & 0x7fffffu);
}

// Paint one launch entry's key art into a fresh RGBA8 buffer, in the same dark
// atmosphere as the panes above. Everything that varies comes from `seed`: the
// glow's tint and place, the ridge line's height and profile. Only base games
// fall back to this, and only when their shipped PNG is missing.
base::Vector<unsigned char> PaintEntryArt(int seed, int W, int H) {
  base::Vector<unsigned char> out(static_cast<size_t>(W) * H * 4);
  const float fw = static_cast<float>(W), fh = static_cast<float>(H);
  const Rgb tints[4] = {
      {0.27f, 0.37f, 0.53f}, {0.41f, 0.33f, 0.51f}, {0.24f, 0.44f, 0.43f}, {0.47f, 0.37f, 0.31f}};
  const Rgb glow = tints[static_cast<int>(Hash2(seed, 1, 17) * 4.f) & 3];
  const float gx = Mixf(0.30f, 0.70f, Hash2(seed, 2, 23));
  const float gy = Mixf(0.28f, 0.52f, Hash2(seed, 3, 29));
  const float ridge = Mixf(0.60f, 0.80f, Hash2(seed, 4, 31));
  for (int y = 0; y < H; ++y) {
    const float fy = static_cast<float>(y);
    const float v = fy / (fh - 1.f);
    for (int x = 0; x < W; ++x) {
      const float fx = static_cast<float>(x);
      const float u = fx / (fw - 1.f);
      const Rgb base0{0.022f, 0.029f, 0.044f}, base1{0.044f, 0.056f, 0.080f};
      Rgb c = Mix(base0, base1, Smooth(0.0f, 1.0f, v));
      c = Add(c, glow, Disc(fx, fy, fw * gx, fh * gy, fh * 0.62f, fh * 0.62f) * 0.26f);
      c = Add(c, Rgb{1.f, 1.f, 1.f}, (Fbm(u * 3.f, v * 4.f, seed + 11, 3) - 0.5f) * 0.02f);

      const float land = ridge + (Fbm(u * 2.4f, 0.5f, seed + 41, 4) - 0.5f) * 0.18f;
      c = Mix(c, Rgb{0.010f, 0.014f, 0.021f}, Smooth(land, land + 0.012f, v));

      const float g = (Hash2(x, y, 9090) - 0.5f) * 0.035f;
      c = Add(c, Rgb{1.f, 1.f, 1.f}, g);
      const float r = std::sqrt((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f)) * 1.7f;
      const float vig = 1.f - 0.28f * Smooth(0.55f, 1.30f, r);
      c.r *= vig;
      c.g *= vig;
      c.b *= vig;

      const size_t o = (static_cast<size_t>(y) * W + x) * 4;
      out[o + 0] = static_cast<unsigned char>(Clamp01(c.r) * 255.f + 0.5f);
      out[o + 1] = static_cast<unsigned char>(Clamp01(c.g) * 255.f + 0.5f);
      out[o + 2] = static_cast<unsigned char>(Clamp01(c.b) * 255.f + 0.5f);
      out[o + 3] = 255;
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// A tiny anti-aliased vector-glyph toolkit. The menu's emblems (the NEXUS star,
// the per-universe marks, the profile sigil, the social icons) are line art that
// rectangles can't express, so they are painted per pixel into transparent RGBA8
// and bound to image widgets. Original geometry; nothing is traced from a real
// logo, so the front screen stays non-infringing.
// ---------------------------------------------------------------------------
struct Glyph {
  base::Vector<unsigned char> px;
  int w = 0, h = 0;
};

void GBlend(Glyph& g, int x, int y, Rgb col, float a) {
  if (x < 0 || y < 0 || x >= g.w || y >= g.h || a <= 0.f)
    return;
  a = Clamp01(a);
  const size_t o = (static_cast<size_t>(y) * g.w + x) * 4;
  const float oa = g.px[o + 3] / 255.f;
  const float na = a + oa * (1.f - a);
  const float src[3] = {col.r, col.g, col.b};
  for (int i = 0; i < 3; ++i) {
    const float oc = g.px[o + i] / 255.f;
    const float nc = na <= 0.f ? 0.f : (src[i] * a + oc * oa * (1.f - a)) / na;
    g.px[o + i] = static_cast<unsigned char>(Clamp01(nc) * 255.f + 0.5f);
  }
  g.px[o + 3] = static_cast<unsigned char>(Clamp01(na) * 255.f + 0.5f);
}
float SegDist(float px, float py, float ax, float ay, float bx, float by) {
  const float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
  const float len = vx * vx + vy * vy;
  const float t = len > 0.f ? Clamp01((wx * vx + wy * vy) / len) : 0.f;
  const float cx = ax + t * vx, cy = ay + t * vy;
  return std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
}
void GLine(Glyph& g, float ax, float ay, float bx, float by, float thick, Rgb col) {
  const int x0 = base::Max(0, static_cast<int>(base::Min(ax, bx) - thick - 1));
  const int x1 = base::Min(g.w - 1, static_cast<int>(base::Max(ax, bx) + thick + 1));
  const int y0 = base::Max(0, static_cast<int>(base::Min(ay, by) - thick - 1));
  const int y1 = base::Min(g.h - 1, static_cast<int>(base::Max(ay, by) + thick + 1));
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x)
      GBlend(g, x, y, col,
             1.f - Smooth(thick - 0.7f, thick + 0.7f, SegDist(x + 0.5f, y + 0.5f, ax, ay, bx, by)));
}
void GRing(Glyph& g, float cx, float cy, float r, float thick, Rgb col) {
  const int x0 = base::Max(0, static_cast<int>(cx - r - thick - 1));
  const int x1 = base::Min(g.w - 1, static_cast<int>(cx + r + thick + 1));
  const int y0 = base::Max(0, static_cast<int>(cy - r - thick - 1));
  const int y1 = base::Min(g.h - 1, static_cast<int>(cy + r + thick + 1));
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) {
      const float d = std::fabs(
          std::sqrt((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy)) - r);
      GBlend(g, x, y, col, 1.f - Smooth(thick - 0.7f, thick + 0.7f, d));
    }
}
void GDisc(Glyph& g, float cx, float cy, float r, Rgb col) {
  const int x0 = base::Max(0, static_cast<int>(cx - r - 1)),
            x1 = base::Min(g.w - 1, static_cast<int>(cx + r + 1));
  const int y0 = base::Max(0, static_cast<int>(cy - r - 1)),
            y1 = base::Min(g.h - 1, static_cast<int>(cy + r + 1));
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) {
      const float d =
          std::sqrt((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
      GBlend(g, x, y, col, 1.f - Smooth(r - 0.8f, r + 0.8f, d));
    }
}
void GGlow(Glyph& g, float cx, float cy, float r, Rgb col, float maxA) {
  const int x0 = base::Max(0, static_cast<int>(cx - r - 1)),
            x1 = base::Min(g.w - 1, static_cast<int>(cx + r + 1));
  const int y0 = base::Max(0, static_cast<int>(cy - r - 1)),
            y1 = base::Min(g.h - 1, static_cast<int>(cy + r + 1));
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) {
      const float d =
          std::sqrt((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
      float a = maxA * (1.f - Clamp01(d / r));
      GBlend(g, x, y, col, a * a);
    }
}
void GTri(Glyph& g, float ax, float ay, float bx, float by, float cx, float cy, Rgb col) {
  const int x0 = base::Max(0, static_cast<int>(base::Min(ax, base::Min(bx, cx)) - 1));
  const int x1 = base::Min(g.w - 1, static_cast<int>(base::Max(ax, base::Max(bx, cx)) + 1));
  const int y0 = base::Max(0, static_cast<int>(base::Min(ay, base::Min(by, cy)) - 1));
  const int y1 = base::Min(g.h - 1, static_cast<int>(base::Max(ay, base::Max(by, cy)) + 1));
  auto edge = [](float px, float py, float x0, float y0, float x1, float y1) {
    return (px - x0) * (y1 - y0) - (py - y0) * (x1 - x0);
  };
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) {
      const float px = x + 0.5f, py = y + 0.5f;
      const float w0 = edge(px, py, bx, by, cx, cy), w1 = edge(px, py, cx, cy, ax, ay),
                  w2 = edge(px, py, ax, ay, bx, by);
      if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0))
        GBlend(g, x, y, col, 1.f);
    }
}
void GEllipse(Glyph& g, float cx, float cy, float rx, float ry, float ang, float thick, Rgb col) {
  const float ca = std::cos(ang), sa = std::sin(ang);
  float pcx = 0, pcy = 0;
  for (int i = 0; i <= 48; ++i) {
    const float t = i / 48.f * 6.2831853f;
    const float ex = rx * std::cos(t), ey = ry * std::sin(t);
    const float x = cx + ex * ca - ey * sa, y = cy + ex * sa + ey * ca;
    if (i > 0)
      GLine(g, pcx, pcy, x, y, thick, col);
    pcx = x;
    pcy = y;
  }
}

// Build every menu emblem, keyed by the image widget it binds to.
base::Vector<base::Pair<base::String, Glyph>> BuildMenuGlyphs() {
  // `tan` was a gold used only by gl_profile. The UI is monochrome, so it is
  // a neutral at the same luminance; see docs/ui-mock/.
  const Rgb light{0.87f, 0.90f, 0.96f}, dim{0.58f, 0.62f, 0.72f}, tan{0.74f, 0.74f, 0.74f};
  const Rgb star{0.93f, 0.96f, 1.0f};
  base::Vector<base::Pair<base::String, Glyph>> out;
  auto make = [&](const char* name, int w, int h) -> Glyph& {
    out.push_back(
        {name, Glyph{base::Vector<unsigned char>(static_cast<size_t>(w) * h * 4, 0), w, h}});
    return out.back().second;
  };

  {  // gl_logo: a three-peak range
    Glyph& g = make("gl_logo", 60, 42);
    GTri(g, 2, 40, 16, 18, 30, 40, light);
    GTri(g, 15, 40, 30, 8, 45, 40, light);
    GTri(g, 31, 40, 45, 20, 58, 40, light);
  }
  {  // gl_nexus: an eight-ray spark with a cool glow (the central wordmark mark)
    Glyph& g = make("gl_nexus", 96, 96);
    GGlow(g, 48, 48, 46, Rgb{0.45f, 0.62f, 0.95f}, 0.10f);
    GGlow(g, 48, 48, 26, star, 0.16f);
    GTri(g, 48, 6, 44, 48, 52, 48, star);
    GTri(g, 48, 90, 44, 48, 52, 48, star);
    GTri(g, 6, 48, 48, 44, 48, 52, star);
    GTri(g, 90, 48, 48, 44, 48, 52, star);
    GTri(g, 68, 28, 50.1f, 50.1f, 45.9f, 45.9f, star);
    GTri(g, 28, 28, 50.1f, 45.9f, 45.9f, 50.1f, star);
    GTri(g, 68, 68, 50.1f, 45.9f, 45.9f, 50.1f, star);
    GTri(g, 28, 68, 50.1f, 50.1f, 45.9f, 45.9f, star);
    GDisc(g, 48, 48, 3.4f, star);
  }
  {  // gl_skyrim: an angular downward crest
    Glyph& g = make("gl_skyrim", 60, 44);
    GLine(g, 8, 8, 52, 8, 2.4f, light);
    GLine(g, 8, 8, 30, 40, 2.4f, light);
    GLine(g, 52, 8, 30, 40, 2.4f, light);
    GLine(g, 30, 8, 30, 23, 2.0f, light);
    GLine(g, 18, 8, 30, 22, 1.7f, light);
    GLine(g, 42, 8, 30, 22, 1.7f, light);
  }
  {  // gl_fallout: an atom (three orbits + nucleus)
    Glyph& g = make("gl_fallout", 64, 64);
    GRing(g, 32, 32, 6, 2.0f, light);
    GEllipse(g, 32, 32, 26, 9, 0.0f, 1.8f, light);
    GEllipse(g, 32, 32, 26, 9, 1.0472f, 1.8f, light);
    GEllipse(g, 32, 32, 26, 9, 2.0944f, 1.8f, light);
    GDisc(g, 32, 32, 3.0f, light);
  }
  {  // gl_starfield: a ringed point (a constellation marker)
    Glyph& g = make("gl_starfield", 52, 52);
    GRing(g, 26, 26, 18, 2.2f, light);
    GDisc(g, 26, 26, 3.4f, light);
  }
  {  // gl_profile: an interlaced four-fold sigil
    Glyph& g = make("gl_profile", 104, 104);
    GRing(g, 52, 52, 48, 3.0f, tan);
    GRing(g, 52, 52, 40, 1.6f, tan);
    GRing(g, 52, 52, 15, 2.2f, tan);
    GLine(g, 52, 8, 52, 96, 1.5f, tan);
    GLine(g, 8, 52, 96, 52, 1.5f, tan);
    for (int k = 0; k < 4; ++k) {
      const float a = k * 1.5707963f;
      GRing(g, 52 + 28 * std::cos(a), 52 + 28 * std::sin(a), 12, 1.6f, tan);
    }
  }
  {  // gl_peers: two figures
    Glyph& g = make("gl_peers", 52, 36);
    GDisc(g, 34, 13, 5, dim);
    GDisc(g, 34, 40, 11, dim);
    GDisc(g, 18, 12, 6, light);
    GDisc(g, 18, 38, 13, light);
  }
  {  // gl_globe
    Glyph& g = make("gl_globe", 44, 44);
    GRing(g, 22, 22, 17, 2.0f, light);
    GEllipse(g, 22, 22, 7, 17, 0.0f, 1.6f, light);
    GLine(g, 6, 22, 38, 22, 1.4f, light);
    GLine(g, 9, 14, 35, 14, 1.3f, light);
    GLine(g, 9, 30, 35, 30, 1.3f, light);
  }
  {  // gl_discord: a community/chat bubble (generic, not a brand mark)
    Glyph& g = make("gl_discord", 48, 38);
    GLine(g, 9, 7, 39, 7, 1.8f, light);
    GLine(g, 9, 27, 30, 27, 1.8f, light);
    GLine(g, 8, 8, 8, 26, 1.8f, light);
    GLine(g, 40, 8, 40, 26, 1.8f, light);
    GTri(g, 13, 27, 13, 35, 24, 27, light);
    GDisc(g, 19, 17, 2.2f, light);
    GDisc(g, 29, 17, 2.2f, light);
  }
  {  // gl_changelog: a document
    Glyph& g = make("gl_changelog", 40, 44);
    GLine(g, 10, 6, 30, 6, 1.8f, light);
    GLine(g, 10, 38, 30, 38, 1.8f, light);
    GLine(g, 10, 6, 10, 38, 1.8f, light);
    GLine(g, 30, 6, 30, 38, 1.8f, light);
    GLine(g, 14, 15, 26, 15, 1.5f, light);
    GLine(g, 14, 22, 26, 22, 1.5f, light);
    GLine(g, 14, 29, 22, 29, 1.5f, light);
  }
  {  // gl_up: a thin up chevron (bottom tagline ornament)
    Glyph& g = make("gl_up", 32, 18);
    GLine(g, 4, 14, 16, 4, 2.0f, dim);
    GLine(g, 28, 14, 16, 4, 2.0f, dim);
  }
  {  // gl_gear
    Glyph& g = make("gl_gear", 44, 44);
    GRing(g, 22, 22, 9, 2.4f, light);
    GRing(g, 22, 22, 3, 1.6f, light);
    for (int k = 0; k < 8; ++k) {
      const float a = k * 0.7853982f, ca = std::cos(a), sa = std::sin(a);
      GLine(g, 22 + 9 * ca, 22 + 9 * sa, 22 + 14 * ca, 22 + 14 * sa, 3.0f, light);
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Procedural hero mesh: a single faceted gem rendered to a clay preview by the
// Thumbnailer and composited at the centre of the menu as the key art.
// ---------------------------------------------------------------------------
struct V3 {
  float x, y, z;
};
inline V3 Vsub(V3 a, V3 b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline V3 Vcross(V3 a, V3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline V3 Vnorm(V3 a) {
  float l = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
  l = l > 1e-6f ? l : 1.f;
  return {a.x / l, a.y / l, a.z / l};
}
void PushVert(asset::MeshLod& m, V3 p, V3 n) {
  asset::Vertex v{};
  v.position[0] = p.x;
  v.position[1] = p.y;
  v.position[2] = p.z;
  v.normal[0] = n.x;
  v.normal[1] = n.y;
  v.normal[2] = n.z;
  v.tangent[0] = 1.f;
  v.tangent[3] = 1.f;
  v.color = 0xffffffffu;
  m.vertices.push_back(v);
}
// Flat-shaded triangle; winding auto-flipped to face away from the origin
// (valid for shapes that are star-shaped about the origin: crystal, cog).
void FaceTri(asset::MeshLod& m, V3 a, V3 b, V3 c) {
  V3 n = Vnorm(Vcross(Vsub(b, a), Vsub(c, a)));
  const V3 cen{(a.x + b.x + c.x) / 3.f, (a.y + b.y + c.y) / 3.f, (a.z + b.z + c.z) / 3.f};
  if (n.x * cen.x + n.y * cen.y + n.z * cen.z < 0.f) {
    base::Swap(b, c);
    n = {-n.x, -n.y, -n.z};
  }
  PushVert(m, a, n);
  PushVert(m, b, n);
  PushVert(m, c, n);
}
asset::Mesh Finalize(asset::MeshLod&& lod) {
  asset::Mesh mesh;
  float r = 0.f;
  for (size_t i = 0; i < lod.vertices.size(); ++i) {
    const asset::Vertex& v = lod.vertices[i];
    r = base::Max(r, std::sqrt(v.position[0] * v.position[0] + v.position[1] * v.position[1] +
                               v.position[2] * v.position[2]));
  }
  // The vertex list is the index list (every tri owns its 3 verts).
  for (size_t i = 0; i < lod.vertices.size(); ++i)
    lod.indices.push_back(static_cast<u32>(i));
  asset::Submesh sm{};
  sm.index_count = static_cast<u32>(lod.indices.size());
  lod.submeshes.push_back(sm);
  mesh.bounds_radius = r;
  mesh.lods.push_back(base::move(lod));
  return mesh;
}

// A faceted icosphere, the menu's single hero centerpiece. `subdiv` levels of
// midpoint subdivision on an icosahedron (flat-shaded for a crisp gem look).
asset::Mesh MakeGem(int subdiv, float radius) {
  const float t = 1.6180340f;
  const V3 base[12] = {{-1, t, 0},  {1, t, 0},  {-1, -t, 0}, {1, -t, 0}, {0, -1, t},  {0, 1, t},
                       {0, -1, -t}, {0, 1, -t}, {t, 0, -1},  {t, 0, 1},  {-t, 0, -1}, {-t, 0, 1}};
  const int faces[20][3] = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11},
                            {1, 5, 9},  {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                            {3, 9, 4},  {3, 4, 2},  {3, 2, 6},   {3, 6, 8},  {3, 8, 9},
                            {4, 9, 5},  {2, 4, 11}, {6, 2, 10},  {8, 6, 7},  {9, 8, 1}};
  base::Vector<base::Array<V3, 3>> tris;
  for (const auto& f : faces)
    tris.push_back({Vnorm(base[f[0]]), Vnorm(base[f[1]]), Vnorm(base[f[2]])});
  auto mid = [](V3 a, V3 b) {
    return Vnorm(V3{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f});
  };
  for (int s = 0; s < subdiv; ++s) {
    base::Vector<base::Array<V3, 3>> next;
    for (const auto& tr : tris) {
      const V3 ab = mid(tr[0], tr[1]), bc = mid(tr[1], tr[2]), ca = mid(tr[2], tr[0]);
      next.push_back({tr[0], ab, ca});
      next.push_back({ab, tr[1], bc});
      next.push_back({ca, bc, tr[2]});
      next.push_back({ab, bc, ca});
    }
    tris.swap(next);
  }
  asset::MeshLod lod;
  auto sc = [&](V3 p) { return V3{p.x * radius, p.y * radius, p.z * radius}; };
  for (const auto& tr : tris)
    FaceTri(lod, sc(tr[0]), sc(tr[1]), sc(tr[2]));
  return Finalize(base::move(lod));
}

}  // namespace

void Engine::GenerateMenuBackdrops() {
#if defined(RECREATION_HAS_UGUI)
  // Portrait third of a 16:9 screen (~0.59:1); stretched to fill each pane.
  const int W = 480, H = 812;
  for (int i = 0; i < 3; ++i) {
    const base::Vector<unsigned char> px = PaintBackdrop(i, W, H);
    const u64 tex = game_ui_.CreateUiTexture(W, H, px.data());
    if (tex) {
      game_ui_.SetMainMenuBackdrop(i, tex);
      RX_INFO("menu backdrop {} painted ({}x{})", GameSlug(menu_universes_[i].game), W, H);
    }
  }
  // Key art per launch entry, from the path BuildMenuEntries resolved: a game's
  // world capture or shipped PNG, a mode's own manifest art.
  const int EW = 384, EH = 288;
  // The plate a mode with no art gets: one flat neutral tone, stretched over the
  // tile. A mode is expected to ship its own image through the manifest's `art`
  // field, and a blank plate says it did not far more honestly than a painting
  // that pretends the mode has a look.
  const unsigned char no_art[4] = {70, 73, 82, 255};
  for (size_t i = 0; i < menu_entries_.size(); ++i) {
    const GameUi::MenuEntry& entry = menu_entries_[i];
    u64 tex = 0;
    const base::String& art = menu_entry_art_[i];
    if (!art.empty()) {
      int w = 0, h = 0, channels = 0;
      if (unsigned char* px = stbi_load(art.c_str(), &w, &h, &channels, 4)) {
        tex = game_ui_.CreateUiTexture(w, h, px);
        stbi_image_free(px);
        RX_INFO("menu key art {} <- {} ({}x{})", entry.title, art, w, h);
      } else {
        RX_WARN("menu key art {} could not be decoded", art);
      }
    }
    if (!tex && entry.kind == GameUi::MenuEntry::Kind::kGame) {
      const base::Vector<unsigned char> px = PaintEntryArt(TitleSeed(entry.title), EW, EH);
      tex = game_ui_.CreateUiTexture(EW, EH, px.data());
      RX_WARN("menu key art {} missing, painted a fallback", entry.title);
    }
    if (!tex) {
      tex = game_ui_.CreateUiTexture(1, 1, no_art);
      RX_INFO("menu key art {} not supplied, flat plate", entry.title);
    }
    if (tex)
      game_ui_.SetMainMenuEntryArt(static_cast<int>(i), tex);
  }

  // Emblems / icons: line art bound to the menu's image widgets.
  for (const auto& [name, g] : BuildMenuGlyphs()) {
    const u64 tex = game_ui_.CreateUiTexture(g.w, g.h, g.px.data());
    if (!tex)
      continue;
    game_ui_.SetMainMenuGlyph(name, tex);
    if (name == "gl_profile")
      game_ui_.SetMainMenuGlyph("gl_profile2", tex);  // sub-screen reuse
  }

  // The menu's key art: a single faceted gem, clay-rendered once and composited
  // at the centre of the screen (mm_hero). Tinted a cool platinum.
  {
    Thumbnailer thumber;
    if (thumber.Init(*renderer_, 640)) {
      const int S = thumber.size();
      base::Vector<unsigned char> px;
      if (thumber.Render(MakeGem(1, 1.0f), px)) {
        const float tint[3] = {0.94f, 0.98f, 1.08f};  // cool platinum
        for (size_t p = 0; p + 3 < px.size(); p += 4) {
          px[p + 0] = static_cast<unsigned char>(base::Min(255.f, px[p + 0] * tint[0]));
          px[p + 1] = static_cast<unsigned char>(base::Min(255.f, px[p + 1] * tint[1]));
          px[p + 2] = static_cast<unsigned char>(base::Min(255.f, px[p + 2] * tint[2]));
        }
        const u64 tex = game_ui_.CreateUiTexture(S, S, px.data());
        if (tex) {
          game_ui_.SetMainMenuGlyph("mm_hero", tex);
          RX_INFO("menu hero gem rendered ({}x{})", S, S);
        }
      }
    } else {
      RX_WARN("menu: thumbnailer unavailable, hero art absent");
    }
  }
#endif
}

void Engine::TickMenuCapture() {
  if (menu_capture_countdown_ <= 0)
    return;
  const int c = menu_capture_countdown_--;  // value before this frame's decrement
  if (c == 5) {                             // hide all overlays a few frames ahead of the grab
    game_ui_.SetHudVisible(false);
    debug_ui_.SetAllVisible(false);
  } else if (c == 2) {  // arm: this frame's RenderFrame writes the clean backbuffer
    renderer_->CaptureScreenshot(menu_capture_path_.c_str());
  } else if (c == 1) {  // restore the overlays for play
    game_ui_.SetHudVisible(true);
    debug_ui_.SetAllVisible(!HideDebugUi);
    RX_INFO("menu backdrop captured: {}", menu_capture_path_);
  }
}

}  // namespace rx
