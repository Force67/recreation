#include "engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#if !defined(_WIN32)
#include <pwd.h>     // getpwuid for the local profile name
#include <unistd.h>  // gethostname / getuid
#endif

#ifndef RECREATION_VERSION
#define RECREATION_VERSION "0.1.0"
#endif

#include "core/log.h"
#include "engine_internal.h"

#include <algorithm>  // std::max / std::clamp in the procedural backdrop painter
#include <cmath>      // std::floor / std::sqrt / std::fabs for the scene noise
#include <utility>    // std::move for the procedural meshes

#include <array>
#include <base/option.h>

#include "asset/mesh.h"
#include "thumbnailer.h"  // off-screen clay render of the hero centerpiece

// The NEXUS main menu: the front door a bare windowed launch opens. Resolves the
// installed universes (Steam/env scan), drives menu navigation and the
// select-then-PLAY request flow, loads a universe on demand, and refreshes the
// front-screen identity/stats plus the cached scene backdrops.
namespace rec {

// Config toggles formerly read from getenv (populated by base::InitOptionsFromEnv).
static base::Option<bool> HideDebugUi{"hide.debug.ui", false, "REC_HIDE_DEBUG_UI"};
static base::Option<bool> MenuCapture{"menu.capture", false, "REC_MENU_CAPTURE"};
static base::Option<const char*> MenuAutoplay{"menu.autoplay", nullptr, "REC_MENU_AUTOPLAY"};

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

// Parse the top `max_items` releases from a Keep-a-Changelog file into NEWS
// entries: each release's first bullet is the headline, and "v<ver> · <date>"
// is the detail line. Returns empty if the file is missing.
static std::vector<MenuNewsItem> ParseChangelog(const std::string& path, int max_items) {
  std::vector<MenuNewsItem> out;
  std::ifstream f(path);
  if (!f) return out;
  auto trim = [](std::string s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    const size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
  };
  std::string line, ver, date;
  bool want_bullet = false;
  while (std::getline(f, line)) {
    if (line.rfind("## ", 0) == 0) {  // release header: "## [x.y.z] - date"
      ver.clear();
      date.clear();
      const size_t lb = line.find('['), rb = line.find(']');
      if (lb != std::string::npos && rb != std::string::npos && rb > lb)
        ver = line.substr(lb + 1, rb - lb - 1);
      const size_t d = line.find("- ", rb == std::string::npos ? 0 : rb);
      if (d != std::string::npos) date = trim(line.substr(d + 2));
      want_bullet = true;
    } else if (want_bullet && line.rfind("- ", 0) == 0) {  // first change = headline
      MenuNewsItem it;
      it.title = trim(line.substr(2));
      it.detail = ver.empty() ? date : ("v" + ver + (date.empty() ? "" : "  \xc2\xb7  " + date));
      out.push_back(std::move(it));
      want_bullet = false;
      if (static_cast<int>(out.size()) >= max_items) break;
    }
  }
  return out;
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
  std::vector<MenuNewsItem> news = ParseChangelog("CHANGELOG.md", 3);
  if (news.empty()) news = {{"Welcome to Recreation", "v" RECREATION_VERSION}};
  self->game_ui_.SetMainMenuNews(news);
  self->GenerateMenuBackdrops();      // original procedural concept art per universe
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
  self->debug_ui_.SetVisible(!HideDebugUi);
  if (!LoadGameData(engine)) {  // boots the managed world, so the game's C# module installs
    REC_ERROR("failed to load universe {}", u.name);
    return;
  }
  // Opt-in (REC_MENU_CAPTURE): grab a clean frame of this world for the menu
  // backdrop cache once it has streamed in, so a later menu shows the real scene.
  // Off by default, since a mid-stream grab can catch an unsettled frame.
  if (!self->config_.headless && MenuCapture) {
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
  if (actions_.pressed(Action::kMenuUp) || in.key_pressed(Key::kW)) game_ui_.MainMenuMove(0, -1);
  if (actions_.pressed(Action::kMenuDown) || in.key_pressed(Key::kS)) game_ui_.MainMenuMove(0, +1);
  if (actions_.pressed(Action::kMenuLeft) || in.key_pressed(Key::kA)) game_ui_.MainMenuMove(-1, 0);
  if (actions_.pressed(Action::kMenuRight) || in.key_pressed(Key::kD)) game_ui_.MainMenuMove(+1, 0);
  if (actions_.pressed(Action::kMenuAccept) || in.key_pressed(Key::kSpace))
    game_ui_.MainMenuActivate();
  if (actions_.pressed(Action::kMenuCancel)) game_ui_.MainMenuBack();

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
    case MainMenuRequest::Kind::kOpenUrl:
      if (!req.url.empty()) {
#if defined(_WIN32)
        const std::string cmd = "start \"\" \"" + req.url + "\"";
#elif defined(__APPLE__)
        const std::string cmd = "open \"" + req.url + "\" >/dev/null 2>&1 &";
#else
        const std::string cmd = "xdg-open \"" + req.url + "\" >/dev/null 2>&1 &";
#endif
        if (std::system(cmd.c_str()) != 0) REC_WARN("could not open url {}", req.url);
        else REC_INFO("opened url {}", req.url);
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

inline float Clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
inline float Mixf(float a, float b, float t) { return a + (b - a) * t; }
inline Rgb Mix(Rgb a, Rgb b, float t) {
  return {Mixf(a.r, b.r, t), Mixf(a.g, b.g, t), Mixf(a.b, b.b, t)};
}
inline Rgb Add(Rgb a, Rgb b, float s) { return {a.r + b.r * s, a.g + b.g * s, a.b + b.b * s}; }
inline float Smooth(float e0, float e1, float x) {
  const float t = Clamp01((x - e0) / (e1 - e0));
  return t * t * (3.f - 2.f * t);
}

// Deterministic value hash in [0,1) — the only randomness the painter uses.
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
std::vector<unsigned char> PaintBackdrop(int universe, int W, int H) {
  std::vector<unsigned char> out(static_cast<size_t>(W) * H * 4);
  const float fw = static_cast<float>(W), fh = static_cast<float>(H);
  for (int y = 0; y < H; ++y) {
    const float fy = static_cast<float>(y);
    const float v = fy / (fh - 1.f);  // 0 top .. 1 bottom
    for (int x = 0; x < W; ++x) {
      const float fx = static_cast<float>(x);
      const float u = fx / (fw - 1.f);
      Rgb c{0.f, 0.f, 0.f};

      // One cohesive dark field shared by all three panes — only a soft,
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

// ---------------------------------------------------------------------------
// A tiny anti-aliased vector-glyph toolkit. The menu's emblems (the NEXUS star,
// the per-universe marks, the profile sigil, the social icons) are line art that
// rectangles can't express, so they are painted per pixel into transparent RGBA8
// and bound to image widgets. Original geometry — nothing is traced from a real
// logo, so the front screen stays non-infringing.
// ---------------------------------------------------------------------------
struct Glyph {
  std::vector<unsigned char> px;
  int w = 0, h = 0;
};

void GBlend(Glyph& g, int x, int y, Rgb col, float a) {
  if (x < 0 || y < 0 || x >= g.w || y >= g.h || a <= 0.f) return;
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
  const int x0 = std::max(0, static_cast<int>(std::min(ax, bx) - thick - 1));
  const int x1 = std::min(g.w - 1, static_cast<int>(std::max(ax, bx) + thick + 1));
  const int y0 = std::max(0, static_cast<int>(std::min(ay, by) - thick - 1));
  const int y1 = std::min(g.h - 1, static_cast<int>(std::max(ay, by) + thick + 1));
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x)
      GBlend(g, x, y, col, 1.f - Smooth(thick - 0.7f, thick + 0.7f, SegDist(x + 0.5f, y + 0.5f, ax, ay, bx, by)));
}
void GRing(Glyph& g, float cx, float cy, float r, float thick, Rgb col) {
  const int x0 = std::max(0, static_cast<int>(cx - r - thick - 1));
  const int x1 = std::min(g.w - 1, static_cast<int>(cx + r + thick + 1));
  const int y0 = std::max(0, static_cast<int>(cy - r - thick - 1));
  const int y1 = std::min(g.h - 1, static_cast<int>(cy + r + thick + 1));
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) {
      const float d = std::fabs(std::sqrt((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy)) - r);
      GBlend(g, x, y, col, 1.f - Smooth(thick - 0.7f, thick + 0.7f, d));
    }
}
void GDisc(Glyph& g, float cx, float cy, float r, Rgb col) {
  const int x0 = std::max(0, static_cast<int>(cx - r - 1)), x1 = std::min(g.w - 1, static_cast<int>(cx + r + 1));
  const int y0 = std::max(0, static_cast<int>(cy - r - 1)), y1 = std::min(g.h - 1, static_cast<int>(cy + r + 1));
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) {
      const float d = std::sqrt((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
      GBlend(g, x, y, col, 1.f - Smooth(r - 0.8f, r + 0.8f, d));
    }
}
void GGlow(Glyph& g, float cx, float cy, float r, Rgb col, float maxA) {
  const int x0 = std::max(0, static_cast<int>(cx - r - 1)), x1 = std::min(g.w - 1, static_cast<int>(cx + r + 1));
  const int y0 = std::max(0, static_cast<int>(cy - r - 1)), y1 = std::min(g.h - 1, static_cast<int>(cy + r + 1));
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) {
      const float d = std::sqrt((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
      float a = maxA * (1.f - Clamp01(d / r));
      GBlend(g, x, y, col, a * a);
    }
}
void GTri(Glyph& g, float ax, float ay, float bx, float by, float cx, float cy, Rgb col) {
  const int x0 = std::max(0, static_cast<int>(std::min({ax, bx, cx}) - 1));
  const int x1 = std::min(g.w - 1, static_cast<int>(std::max({ax, bx, cx}) + 1));
  const int y0 = std::max(0, static_cast<int>(std::min({ay, by, cy}) - 1));
  const int y1 = std::min(g.h - 1, static_cast<int>(std::max({ay, by, cy}) + 1));
  auto edge = [](float px, float py, float x0, float y0, float x1, float y1) {
    return (px - x0) * (y1 - y0) - (py - y0) * (x1 - x0);
  };
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) {
      const float px = x + 0.5f, py = y + 0.5f;
      const float w0 = edge(px, py, bx, by, cx, cy), w1 = edge(px, py, cx, cy, ax, ay), w2 = edge(px, py, ax, ay, bx, by);
      if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0)) GBlend(g, x, y, col, 1.f);
    }
}
void GEllipse(Glyph& g, float cx, float cy, float rx, float ry, float ang, float thick, Rgb col) {
  const float ca = std::cos(ang), sa = std::sin(ang);
  float pcx = 0, pcy = 0;
  for (int i = 0; i <= 48; ++i) {
    const float t = i / 48.f * 6.2831853f;
    const float ex = rx * std::cos(t), ey = ry * std::sin(t);
    const float x = cx + ex * ca - ey * sa, y = cy + ex * sa + ey * ca;
    if (i > 0) GLine(g, pcx, pcy, x, y, thick, col);
    pcx = x;
    pcy = y;
  }
}

// Build every menu emblem, keyed by the image widget it binds to.
std::vector<std::pair<std::string, Glyph>> BuildMenuGlyphs() {
  const Rgb light{0.87f, 0.90f, 0.96f}, dim{0.58f, 0.62f, 0.72f}, tan{0.82f, 0.73f, 0.56f};
  const Rgb star{0.93f, 0.96f, 1.0f};
  std::vector<std::pair<std::string, Glyph>> out;
  auto make = [&](const char* name, int w, int h) -> Glyph& {
    out.push_back({name, Glyph{std::vector<unsigned char>(static_cast<size_t>(w) * h * 4, 0), w, h}});
    return out.back().second;
  };

  {  // gl_logo — a three-peak range
    Glyph& g = make("gl_logo", 60, 42);
    GTri(g, 2, 40, 16, 18, 30, 40, light);
    GTri(g, 15, 40, 30, 8, 45, 40, light);
    GTri(g, 31, 40, 45, 20, 58, 40, light);
  }
  {  // gl_nexus — an eight-ray spark with a cool glow (the central wordmark mark)
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
  {  // gl_skyrim — an angular downward crest
    Glyph& g = make("gl_skyrim", 60, 44);
    GLine(g, 8, 8, 52, 8, 2.4f, light);
    GLine(g, 8, 8, 30, 40, 2.4f, light);
    GLine(g, 52, 8, 30, 40, 2.4f, light);
    GLine(g, 30, 8, 30, 23, 2.0f, light);
    GLine(g, 18, 8, 30, 22, 1.7f, light);
    GLine(g, 42, 8, 30, 22, 1.7f, light);
  }
  {  // gl_fallout — an atom (three orbits + nucleus)
    Glyph& g = make("gl_fallout", 64, 64);
    GRing(g, 32, 32, 6, 2.0f, light);
    GEllipse(g, 32, 32, 26, 9, 0.0f, 1.8f, light);
    GEllipse(g, 32, 32, 26, 9, 1.0472f, 1.8f, light);
    GEllipse(g, 32, 32, 26, 9, 2.0944f, 1.8f, light);
    GDisc(g, 32, 32, 3.0f, light);
  }
  {  // gl_starfield — a ringed point (a constellation marker)
    Glyph& g = make("gl_starfield", 52, 52);
    GRing(g, 26, 26, 18, 2.2f, light);
    GDisc(g, 26, 26, 3.4f, light);
  }
  {  // gl_profile — an interlaced four-fold sigil
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
  {  // gl_peers — two figures
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
  {  // gl_discord — a community/chat bubble (generic, not a brand mark)
    Glyph& g = make("gl_discord", 48, 38);
    GLine(g, 9, 7, 39, 7, 1.8f, light);
    GLine(g, 9, 27, 30, 27, 1.8f, light);
    GLine(g, 8, 8, 8, 26, 1.8f, light);
    GLine(g, 40, 8, 40, 26, 1.8f, light);
    GTri(g, 13, 27, 13, 35, 24, 27, light);
    GDisc(g, 19, 17, 2.2f, light);
    GDisc(g, 29, 17, 2.2f, light);
  }
  {  // gl_changelog — a document
    Glyph& g = make("gl_changelog", 40, 44);
    GLine(g, 10, 6, 30, 6, 1.8f, light);
    GLine(g, 10, 38, 30, 38, 1.8f, light);
    GLine(g, 10, 6, 10, 38, 1.8f, light);
    GLine(g, 30, 6, 30, 38, 1.8f, light);
    GLine(g, 14, 15, 26, 15, 1.5f, light);
    GLine(g, 14, 22, 26, 22, 1.5f, light);
    GLine(g, 14, 29, 22, 29, 1.5f, light);
  }
  {  // gl_up — a thin up chevron (bottom tagline ornament)
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
// Procedural hero mesh — a single faceted gem rendered to a clay preview by the
// Thumbnailer and composited at the centre of the menu as the key art.
// ---------------------------------------------------------------------------
struct V3 {
  float x, y, z;
};
inline V3 Vsub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
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
  v.position[0] = p.x; v.position[1] = p.y; v.position[2] = p.z;
  v.normal[0] = n.x; v.normal[1] = n.y; v.normal[2] = n.z;
  v.tangent[0] = 1.f; v.tangent[3] = 1.f; v.color = 0xffffffffu;
  m.vertices.push_back(v);
}
// Flat-shaded triangle; winding auto-flipped to face away from the origin
// (valid for shapes that are star-shaped about the origin — crystal, cog).
void FaceTri(asset::MeshLod& m, V3 a, V3 b, V3 c) {
  V3 n = Vnorm(Vcross(Vsub(b, a), Vsub(c, a)));
  const V3 cen{(a.x + b.x + c.x) / 3.f, (a.y + b.y + c.y) / 3.f, (a.z + b.z + c.z) / 3.f};
  if (n.x * cen.x + n.y * cen.y + n.z * cen.z < 0.f) {
    std::swap(b, c);
    n = {-n.x, -n.y, -n.z};
  }
  PushVert(m, a, n); PushVert(m, b, n); PushVert(m, c, n);
}
asset::Mesh Finalize(asset::MeshLod&& lod) {
  asset::Mesh mesh;
  float r = 0.f;
  for (size_t i = 0; i < lod.vertices.size(); ++i) {
    const asset::Vertex& v = lod.vertices[i];
    r = std::max(r, std::sqrt(v.position[0] * v.position[0] + v.position[1] * v.position[1] +
                              v.position[2] * v.position[2]));
  }
  // The vertex list is the index list (every tri owns its 3 verts).
  for (size_t i = 0; i < lod.vertices.size(); ++i) lod.indices.push_back(static_cast<u32>(i));
  asset::Submesh sm{};
  sm.index_count = static_cast<u32>(lod.indices.size());
  lod.submeshes.push_back(sm);
  mesh.bounds_radius = r;
  mesh.lods.push_back(std::move(lod));
  return mesh;
}

// A faceted icosphere — the menu's single hero centerpiece. `subdiv` levels of
// midpoint subdivision on an icosahedron (flat-shaded for a crisp gem look).
asset::Mesh MakeGem(int subdiv, float radius) {
  const float t = 1.6180340f;
  const V3 base[12] = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0},
                       {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t},
                       {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
  const int faces[20][3] = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11},
                            {1, 5, 9},  {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                            {3, 9, 4},  {3, 4, 2},  {3, 2, 6},   {3, 6, 8},  {3, 8, 9},
                            {4, 9, 5},  {2, 4, 11}, {6, 2, 10},  {8, 6, 7},  {9, 8, 1}};
  std::vector<std::array<V3, 3>> tris;
  for (const auto& f : faces)
    tris.push_back({Vnorm(base[f[0]]), Vnorm(base[f[1]]), Vnorm(base[f[2]])});
  auto mid = [](V3 a, V3 b) { return Vnorm(V3{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f}); };
  for (int s = 0; s < subdiv; ++s) {
    std::vector<std::array<V3, 3>> next;
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
  for (const auto& tr : tris) FaceTri(lod, sc(tr[0]), sc(tr[1]), sc(tr[2]));
  return Finalize(std::move(lod));
}

}  // namespace

void Engine::GenerateMenuBackdrops() {
#if defined(RECREATION_HAS_UGUI)
  // Portrait third of a 16:9 screen (~0.59:1); stretched to fill each pane.
  const int W = 480, H = 812;
  for (int i = 0; i < 3; ++i) {
    const std::vector<unsigned char> px = PaintBackdrop(i, W, H);
    const u64 tex = game_ui_.CreateUiTexture(W, H, px.data());
    if (tex) {
      game_ui_.SetMainMenuBackdrop(i, tex);
      REC_INFO("menu backdrop {} painted ({}x{})", GameSlug(menu_universes_[i].game), W, H);
    }
  }
  // Emblems / icons — line art bound to the menu's image widgets.
  for (const auto& [name, g] : BuildMenuGlyphs()) {
    const u64 tex = game_ui_.CreateUiTexture(g.w, g.h, g.px.data());
    if (!tex) continue;
    game_ui_.SetMainMenuGlyph(name, tex);
    if (name == "gl_profile") game_ui_.SetMainMenuGlyph("gl_profile2", tex);  // sub-screen reuse
  }

  // The menu's key art: a single faceted gem, clay-rendered once and composited
  // at the centre of the screen (mm_hero). Tinted a cool platinum.
  {
    Thumbnailer thumber;
    if (thumber.Init(renderer_, 640)) {
      const int S = thumber.size();
      std::vector<unsigned char> px;
      if (thumber.Render(MakeGem(1, 1.0f), px)) {
        const float tint[3] = {0.94f, 0.98f, 1.08f};  // cool platinum
        for (size_t p = 0; p + 3 < px.size(); p += 4) {
          px[p + 0] = static_cast<unsigned char>(std::min(255.f, px[p + 0] * tint[0]));
          px[p + 1] = static_cast<unsigned char>(std::min(255.f, px[p + 1] * tint[1]));
          px[p + 2] = static_cast<unsigned char>(std::min(255.f, px[p + 2] * tint[2]));
        }
        const u64 tex = game_ui_.CreateUiTexture(S, S, px.data());
        if (tex) {
          game_ui_.SetMainMenuGlyph("mm_hero", tex);
          REC_INFO("menu hero gem rendered ({}x{})", S, S);
        }
      }
    } else {
      REC_WARN("menu: thumbnailer unavailable, hero art absent");
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
    debug_ui_.SetAllVisible(!HideDebugUi);
    REC_INFO("menu backdrop captured: {}", menu_capture_path_);
  }
}

}  // namespace rec
