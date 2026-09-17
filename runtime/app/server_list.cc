#include "runtime/app/server_list.h"

#if RECREATION_HAS_NET

#include <base/option.h>

#include <cstdio>

#include "components/bethesda/game_profile.h"
#include "components/bethesda/load_order.h"
#include "components/masterlist/load_order_digest.h"
#include "core/log.h"
#include "runtime/app/engine.h"

#ifndef RECREATION_VERSION
#define RECREATION_VERSION "0.1.0"
#endif

namespace rx {
namespace {

static base::Option<const char*> MasterlistOpt{
    "masterlist.url", nullptr, "RX_MASTERLIST",
    "server list to announce a hosted session to and to browse"};

// The list's id for a world: lowercase and stable, because it is a filter value
// in somebody else's browser, not a label anybody reads.
const char* DomainId(bethesda::Game game) {
  switch (game) {
    case bethesda::Game::kSkyrimSe: return "skyrim";
    case bethesda::Game::kFallout4: return "fallout4";
    case bethesda::Game::kFallout76: return "fallout76";
    case bethesda::Game::kFallout3: return "fallout3";
    case bethesda::Game::kFalloutNv: return "falloutnv";
    case bethesda::Game::kStarfield: return "starfield";
    case bethesda::Game::kOblivion: return "oblivion";
    case bethesda::Game::kMorrowind: return "morrowind";
    default: return "";
  }
}

base::String Decimal(u64 value) {
  char buffer[24] = {};
  std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
  return base::String(buffer);
}

base::String Slots(u32 players, u32 max_players) {
  return Decimal(players) + " / " + Decimal(max_players);
}

base::String Megabytes(u64 bytes) {
  char buffer[32] = {};
  if (bytes >= 1024ull * 1024)
    std::snprintf(buffer, sizeof(buffer), "%.0f MB", double(bytes) / (1024.0 * 1024.0));
  else
    std::snprintf(buffer, sizeof(buffer), "%.0f KB", double(bytes) / 1024.0);
  return base::String(buffer);
}

base::String Uptime(u64 seconds) {
  if (seconds < 90)
    return base::String("just opened");
  if (seconds < 3600)
    return base::String("open ") + Decimal(seconds / 60) + " minutes";
  const u64 hours = seconds / 3600;
  return base::String("open ") + Decimal(hours) + (hours == 1 ? " hour" : " hours");
}

// The gametype as a row reads: the coop ruleset has a name of its own, anything
// else is whatever the mode called itself, capitalised.
base::String PrettyGametype(const base::String& gametype) {
  if (gametype.empty() || gametype == "campaign-coop")
    return base::String("Campaign  ·  co-op");
  base::String out = gametype;
  if (out[0] >= 'a' && out[0] <= 'z')
    out[0] = static_cast<char>(out[0] - 'a' + 'A');
  return out;
}

// What this machine would load for that world, digested the way the host
// digested its own: same parser, same game profile, so a mismatch means the
// worlds really differ and not that the two sides counted differently.
base::String LocalDigest(bethesda::Game game, const base::String& plugins_txt) {
  if (game == bethesda::Game::kUnknown || plugins_txt.empty())
    return base::String();
  const bethesda::LoadOrder order =
      bethesda::LoadOrder::FromPluginsTxt(plugins_txt, bethesda::GameProfile::For(game));
  if (order.plugins().empty())
    return base::String();
  return masterlist::LoadOrderDigest(order.plugins());
}

base::String ServerName(const base::String& configured, const base::String& player) {
  if (!configured.empty())
    return configured;
  const base::String who = player.empty() ? base::String("somebody") : player;
  return who + "'s session";
}

}  // namespace

base::String MasterlistUrl(const Engine& engine) {
  if (!engine.config_.masterlist_url.empty())
    return engine.config_.masterlist_url;
  if (const char* env = MasterlistOpt.get(); env != nullptr && *env != '\0')
    return base::String(env);
  return base::String();
}

void StartServerAnnounce(Engine& engine) {
  Engine* const self = &engine;
  if (!self->config_.announce || self->server_session_ == nullptr)
    return;
  if (self->announcer_.running())
    return;

  const base::String url = MasterlistUrl(engine);
  if (url.empty()) {
    RX_INFO(
        "masterlist: no list configured (--masterlist or RX_MASTERLIST), "
        "the session runs unlisted");
    return;
  }

  masterlist::ServerInfo info;
  info.port = self->config_.port;
  info.name = ServerName(self->config_.server_name, self->config_.player_name);
  info.domain = DomainId(self->game_);
  // A mode is its own gametype: the base rules are the coop campaign, anything
  // armed on top names the row it produces in the browser.
  info.mode_id = self->menu_mode_id_;
  info.gametype = self->menu_mode_id_.empty() ? base::String("campaign-coop")
                                              : self->menu_mode_id_;
  info.version = RECREATION_VERSION;
  info.max_players = self->config_.max_clients;
  info.players = self->server_session_->client_count();
  info.plugins = masterlist::LoadOrderDigest(self->load_order_plugins_);
  info.plugin_count = static_cast<u32>(self->load_order_plugins_.size());
  if (self->mod_catalog_) {
    info.resources = static_cast<u32>(self->mod_catalog_->manifest().resources.size());
    info.resources_bytes = self->mod_catalog_->manifest().TotalBytes();
  }

  if (info.domain.empty()) {
    RX_WARN("masterlist: this game has no list id, the session runs unlisted");
    return;
  }

  // The count is read from the announcer's thread every beat, so it goes
  // through the atomic the net tick refreshes rather than through the session.
  self->announced_players_.store(info.players);
  self->announcer_.Start(url, info, [self] { return self->announced_players_.load(); });
}

void StopServerAnnounce(Engine& engine) {
  engine.announcer_.Stop();
}

void RequestServerList(Engine& engine) {
  Engine* const self = &engine;
  const base::String url = MasterlistUrl(engine);
  if (url.empty()) {
    self->game_ui_.SetMainMenuServers(
        {}, "No server list configured  ·  pass --masterlist or set RX_MASTERLIST");
    return;
  }

  masterlist::ListQuery query;
  // No server-side filter: the screen's own tabs cut the list, and a player
  // with two games installed wants to see both.
  query.limit = 200;
  if (self->server_query_.Start(url, query))
    self->game_ui_.SetMainMenuServers({}, "Looking for sessions...");
}

void PollServerList(Engine& engine) {
  Engine* const self = &engine;
  masterlist::ListResult result;
  if (!self->server_query_.Poll(&result))
    return;

  if (!result.ok) {
    // Say which of the two it is. A browser that shows nothing for an
    // unreachable list teaches players that nobody plays this game.
    self->game_ui_.SetMainMenuServers({}, base::String("Could not reach the server list  ·  ") +
                                              result.error);
    RX_WARN("masterlist: list query failed ({})", result.error.c_str());
    return;
  }

  // One digest per universe, computed once for the whole page rather than once
  // per row: every row of a world compares against the same local install.
  const int universes = static_cast<int>(self->menu_universes_.size());
  base::Vector<base::String> digests;
  for (int i = 0; i < universes; ++i) {
    const auto& u = self->menu_universes_[i];
    digests.push_back(u.available ? LocalDigest(u.game, u.plugins_txt) : base::String());
  }

  base::Vector<GameUi::MenuServer> rows;
  for (mem_size i = 0; i < result.servers.size(); ++i) {
    const masterlist::ServerEntry& entry = result.servers[i];
    int universe = -1;
    for (int u = 0; u < universes && universe < 0; ++u) {
      const char* id = DomainId(self->menu_universes_[u].game);
      if (*id != '\0' && entry.domain == id)
        universe = u;
    }
    const bool installed = universe >= 0 && self->menu_universes_[universe].available;

    GameUi::MenuServer row;
    row.name = entry.name;
    row.world = universe >= 0 ? self->menu_universes_[universe].name : entry.domain;
    row.gametype = PrettyGametype(entry.gametype);
    row.players = Slots(entry.players, entry.max_players);
    // Deliberately blank: only this machine can measure its own round trip to
    // that host, and nothing does yet. A number from the list would be a lie.
    row.ping = "";
    row.entry = entry.passworded ? base::String("Password")
                                 : (entry.has_slots() ? base::String("Open")
                                                      : base::String("Full"));
    row.address = entry.address;
    row.host = entry.verified ? base::String("Reachable") : base::String("Unverified");
    row.detail = row.world + "  ·  " + Uptime(entry.uptime_secs);
    row.arrive = "Your character, out of your own save";
    row.progress = "Yours, in your own save";
    row.note = "Before you can enter";
    row.universe = universe >= 0 ? universe : 0;

    if (!installed) {
      row.requirements.push_back(
          GameUi::MenuRequirement{row.world + " is not installed", "Missing", false});
    } else {
      const base::String& local = digests[universe];
      const bool known = !local.empty() && !entry.plugins.empty();
      const bool matches = known && local == entry.plugins;
      base::String label = Decimal(entry.plugin_count) + " plugins";
      if (!known)
        row.requirements.push_back(GameUi::MenuRequirement{label, "Unknown", true});
      else
        row.requirements.push_back(
            GameUi::MenuRequirement{label, matches ? "Match" : "Different", matches});
    }
    if (entry.resources > 0) {
      row.requirements.push_back(GameUi::MenuRequirement{
          Decimal(entry.resources) + " resources  ·  " + Megabytes(entry.resources_bytes),
          "Streams on join", true});
    }

    // A passworded session is listed but not dialable: there is nowhere to type
    // a password yet, so offering the join would only fail at the handshake.
    row.joinable = installed && entry.has_slots() && !entry.passworded;
    rows.push_back(base::move(row));
  }

  base::String status;
  if (rows.empty()) {
    status = "Nobody is hosting right now  ·  press HOST to be the first";
  } else {
    status = Decimal(rows.size()) + (rows.size() == 1 ? " session" : " sessions") +
             " open  ·  joining takes the host's world, your saves stay yours";
  }
  self->game_ui_.SetMainMenuServers(rows, status);
}

}  // namespace rx

#endif  // RECREATION_HAS_NET
