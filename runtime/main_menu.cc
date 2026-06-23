#include "engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#if !defined(_WIN32)
#include <pwd.h>     // getpwuid for the local profile name
#include <unistd.h>  // gethostname / getuid
#endif

#ifndef RECREATION_VERSION
#define RECREATION_VERSION "0.1.0"
#endif

#include "core/log.h"
#include "engine_internal.h"

#if defined(RECREATION_HAS_UGUI)
#include <stb_image.h>  // load cached menu backdrops (windowed client only)
#endif

// The NEXUS main menu: the front door a bare windowed launch opens. Resolves the
// installed universes (Steam/env scan), drives menu navigation and the
// select-then-PLAY request flow, loads a universe on demand, and refreshes the
// front-screen identity/stats plus the cached scene backdrops.
namespace rec {

// The C# gameplay modules each universe installs once it is the primary domain,
// previewed on the menu's Mods screen before the game loads. Skyrim and
// Starfield ship a full layer (SkyrimMod / StarfieldMod); Fallout 4 runs the
// shared SDK for now.
static std::vector<std::string> MenuModulesFor(int universe) {
  switch (universe) {
    case 0:
      return {"Attribute Regeneration", "Quest Progress",     "Combat Tracker",
              "Essential Protection",    "Injury Slowdown",    "Time of Day",
              "Encumbrance",             "Location Discovery", "Harvesting",
              "Book Learning",           "Racial Abilities",   "Blessing Upkeep",
              "Vampirism",               "Lycanthropy",        "Shout Cooldown"};
    case 2:
      return {"Oxygen / CO2", "Environmental Hazards", "Mass Encumbrance", "Well Rested",
              "Quest Rewards", "Combat Rewards",       "Discovery XP",     "Notifications"};
    default:
      return {"Recreation SDK", "Event Bus", "Fallout 4 content domain"};
  }
}

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
      {bethesda::Game::kSkyrimSe, "Skyrim", "REC_SKYRIM_DATA", "Skyrim Special Edition/Data"},
      {bethesda::Game::kFallout4, "Fallout 4", "REC_FALLOUT4_DATA", "Fallout 4/Data"},
      {bethesda::Game::kStarfield, "Starfield", "REC_STARFIELD_DATA", "Starfield/Data"},
  };
  // Steam "common" roots to scan when no explicit path is configured.
  const char* roots[] = {
      "/speed/SteamLibrary/steamapps/common",
      "/home/vince/.local/share/Steam/steamapps/common",
      "/home/vince/.steam/steam/steamapps/common",
  };
  auto from_config = [&](bethesda::Game g) -> std::pair<std::string, std::string> {
    if (self->config_.game == g && !self->config_.data_dir.empty())
      return {self->config_.data_dir, self->config_.plugins_txt};
    for (const auto& d : self->config_.extra_domains)
      if (d.game == g && !d.data_dir.empty()) return {d.data_dir, d.plugins_txt};
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
      if (const char* e = std::getenv(specs[i].env)) u.data_dir = e;  // env override
    if (u.data_dir.empty()) {                                         // Steam scan
      for (const char* root : roots) {
        std::error_code ec;
        fs::path p = fs::path(root) / specs[i].subdir;
        if (fs::exists(p, ec)) {
          u.data_dir = p.string();
          break;
        }
      }
    }
    if (u.plugins_txt.empty() && !u.data_dir.empty()) u.plugins_txt = u.data_dir + "/../plugins.txt";
    std::error_code ec;
    u.available = !u.data_dir.empty() && fs::exists(u.data_dir, ec);
    REC_INFO("menu universe {}: {} -> {}", i, u.name,
             u.available ? u.data_dir : std::string("(unavailable)"));
  }
}

void SetupMainMenu(Engine& engine) {
  Engine* const self = &engine;
  self->main_menu_active_ = true;
  ResolveUniverses(engine);
  std::vector<std::string> names;
  std::vector<bool> avail;
  for (const Engine::MenuUniverse& u : self->menu_universes_) {
    names.push_back(u.name);
    avail.push_back(u.available);
  }
  self->game_ui_.SetMainMenuUniverses(names, avail);
  self->game_ui_.OpenMainMenu();
  self->LoadMenuBackdrops();          // real scenes from past playthroughs, if cached
  self->debug_ui_.SetVisible(false);  // a clean front screen, no debug overlays
  REC_INFO("nexus main menu open");
}

void EnterUniverse(Engine& engine, int idx, bool multiplayer, bool host, const std::string& join_address) {
  Engine* const self = &engine;
  if (idx < 0 || idx >= static_cast<int>(self->menu_universes_.size())) return;
  const Engine::MenuUniverse& u = self->menu_universes_[idx];
  if (!u.available) {
    REC_WARN("universe {} has no data; cannot enter", u.name);
    return;
  }
  REC_INFO("entering universe {}{}", u.name,
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
  self->game_ui_.CloseMainMenu();
  self->main_menu_active_ = false;
  self->debug_ui_.SetVisible(std::getenv("REC_HIDE_DEBUG_UI") == nullptr);
  if (!LoadGameData(engine)) {  // boots the managed world, so the game's C# module installs
    REC_ERROR("failed to load universe {}", u.name);
    return;
  }
  // Opt-in (REC_MENU_CAPTURE): grab a clean frame of this world for the menu
  // backdrop cache once it has streamed in, so a later menu shows the real scene.
  // Off by default, since a mid-stream grab can catch an unsettled frame.
  if (!self->config_.headless && std::getenv("REC_MENU_CAPTURE")) {
    self->menu_capture_path_ = "thumbs/menu_" + GameSlug(self->config_.game) + ".png";
    self->menu_capture_countdown_ = 600;  // ~10s at 60fps to let streaming + RT settle
  }
#if RECREATION_HAS_NET
  if (self->config_.host_server || !self->config_.connect_address.empty()) {
    if (!StartNetworking(engine)) REC_WARN("networking failed to start");
  }
#endif
}

void Engine::UpdateMainMenu(f32 dt) {
  (void)dt;
  const InputState& in = window_->input();
  window_->SetRelativeMouseMode(false);  // free cursor so the menu can be clicked

  // Test hook: REC_MENU_AUTOPLAY=<0|1|2> drives the same select-then-PLAY path a
  // mouse/keyboard would, so the menu->request->boot chain runs without input.
  if (const char* ap = std::getenv("REC_MENU_AUTOPLAY")) {
    static int beat = 0;
    if (++beat == 45) {
      game_ui_.MainMenuMove(std::atoi(ap) - game_ui_.selected_universe(), 0);  // pick the column
      game_ui_.MainMenuActivate();  // PLAY -> kEnterUniverse, dispatched by the poll below
    }
  }

  if (in.key_pressed(Key::kW)) game_ui_.MainMenuMove(0, -1);
  if (in.key_pressed(Key::kS)) game_ui_.MainMenuMove(0, +1);
  if (in.key_pressed(Key::kA)) game_ui_.MainMenuMove(-1, 0);
  if (in.key_pressed(Key::kD)) game_ui_.MainMenuMove(+1, 0);
  if (in.key_pressed(Key::kReturn) || in.key_pressed(Key::kSpace)) game_ui_.MainMenuActivate();
  if (in.key_pressed(Key::kEscape)) game_ui_.MainMenuBack();

  RefreshMenuData();

  const MainMenuRequest req = game_ui_.PollMainMenuRequest();
  switch (req.kind) {
    case MainMenuRequest::Kind::kEnterUniverse:
      EnterUniverse(*this, req.universe, false, false, "");
      break;
    case MainMenuRequest::Kind::kHostServer:
      EnterUniverse(*this, req.universe, true, true, "");
      break;
    case MainMenuRequest::Kind::kJoinServer:
      EnterUniverse(*this, req.universe, true, false,
                    req.address.empty() ? std::string("127.0.0.1") : req.address);
      break;
    case MainMenuRequest::Kind::kQuit:
      RequestQuit();
      break;
    case MainMenuRequest::Kind::kNone:
      break;
  }
}

void Engine::RefreshMenuData() {
  MainMenuStats stats;

  // Real local-profile identity for the front screen: the OS account and host,
  // plus the configured multiplayer handle (falls back to the login name).
  std::string login;
  char host[256] = {0};
#if !defined(_WIN32)
  if (const struct passwd* pw = getpwuid(getuid()); pw && pw->pw_name) login = pw->pw_name;
  if (gethostname(host, sizeof(host) - 1) != 0) host[0] = '\0';
#endif
  if (login.empty()) {
    if (const char* u = std::getenv("USER")) login = u;
    else if (const char* u2 = std::getenv("USERNAME")) login = u2;
  }
  if (!host[0]) {
    if (const char* h = std::getenv("HOSTNAME")) std::snprintf(host, sizeof(host), "%s", h);
    else if (const char* h2 = std::getenv("COMPUTERNAME")) std::snprintf(host, sizeof(host), "%s", h2);
  }

  const std::string handle(config_.player_name.c_str());
  stats.account = login.empty() ? "local" : login;
  stats.machine = host[0] ? host : "this machine";
  stats.build = RECREATION_VERSION;
  stats.player_name = (handle.empty() || handle == "player") ? stats.account : handle;
  stats.level = 0;
  stats.net_status = "Offline";

  int avail = 0;
  for (const auto& u : menu_universes_)
    if (u.available) ++avail;
  stats.universes_available = avail;

#if RECREATION_HAS_NET
  if (server_session_) {
    stats.players_online = static_cast<int>(server_session_->client_count());
    stats.net_status = "Hosting :" + std::to_string(config_.port);
  } else if (client_session_) {
    stats.net_status = client_session_->joined() ? "Connected" : "Connecting";
  }
#endif
  game_ui_.SetMainMenuStats(stats);
  game_ui_.SetMainMenuMods(MenuModulesFor(game_ui_.selected_universe()));
}

void Engine::LoadMenuBackdrops() {
#if defined(RECREATION_HAS_UGUI)
  namespace fs = std::filesystem;
  for (int i = 0; i < 3; ++i) {
    const std::string slug = GameSlug(menu_universes_[i].game);
    const std::string cands[] = {"thumbs/menu_" + slug + ".png", "assets/menu/" + slug + ".png",
                                 "assets/menu/" + slug + ".jpg"};
    for (const std::string& path : cands) {
      std::error_code ec;
      if (!fs::exists(path, ec)) continue;
      int w = 0, h = 0, n = 0;
      unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 4);
      if (!px) continue;
      // A captured frame is full-screen (16:9); the column is a portrait third,
      // so centre-crop to the column's aspect instead of squishing the scene.
      const int cw = std::min(w, static_cast<int>(h * 640.0f / 1080.0f + 0.5f));
      const int cx = (w - cw) / 2;
      std::vector<unsigned char> crop(static_cast<size_t>(cw) * h * 4);
      for (int y = 0; y < h; ++y)
        std::memcpy(crop.data() + static_cast<size_t>(y) * cw * 4,
                    px + (static_cast<size_t>(y) * w + cx) * 4, static_cast<size_t>(cw) * 4);
      stbi_image_free(px);
      const u64 tex = game_ui_.CreateUiTexture(cw, h, crop.data());
      if (tex) {
        game_ui_.SetMainMenuBackdrop(i, tex);
        REC_INFO("menu backdrop {}: {} ({}x{} crop {}x{})", slug, path, w, h, cw, h);
      }
      break;
    }
  }
#endif
}

void Engine::TickMenuCapture() {
  if (menu_capture_countdown_ <= 0) return;
  const int c = menu_capture_countdown_--;  // value before this frame's decrement
  if (c == 5) {                             // hide all overlays a few frames ahead of the grab
    game_ui_.SetHudVisible(false);
    debug_ui_.SetAllVisible(false);
  } else if (c == 2) {  // arm: this frame's RenderFrame writes the clean backbuffer
    renderer_.CaptureScreenshot(menu_capture_path_);
  } else if (c == 1) {  // restore the overlays for play
    game_ui_.SetHudVisible(true);
    debug_ui_.SetAllVisible(std::getenv("REC_HIDE_DEBUG_UI") == nullptr);
    REC_INFO("menu backdrop captured: {}", menu_capture_path_);
  }
}

}  // namespace rec
