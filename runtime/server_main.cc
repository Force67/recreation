// Dedicated server runner: headless engine, fixed tick, no renderer. The
// world simulates exactly like a listen server, clients receive it through
// snapshots.

#include <csignal>
#include <cstring>
#include <string>

#include "core/log.h"
#include "engine.h"

namespace {

rx::Engine* g_engine = nullptr;

void HandleSignal(int) {
  if (g_engine) g_engine->RequestQuit();
}

#ifndef _WIN32
// SIGHUP asks the running server to re-scan its mods directory and re-offer the
// updated set to joining clients, so an author can iterate without a restart.
void HandleReload(int) {
  if (g_engine) g_engine->RequestModReload();
}
#endif

void PrintUsage() {
  RX_INFO("usage: recreation-server [options]");
  RX_INFO("  --data-dir <path>     game Data directory (omit for demo scene)");
  RX_INFO("  --plugins <path>      plugins.txt (default: <data-dir>/../plugins.txt)");
  RX_INFO("  --game <id>           skyrimse | fo4 | fo76 (default: autodetect)");
  RX_INFO("  --port <port>         listen port (default: 29700)");
  RX_INFO("  --max-clients <n>     player slots (default: 64)");
  RX_INFO("  --mods-dir <path>     UGC resources to stream to clients (FiveM-style)");
}

rx::bethesda::Game ParseGame(const std::string& id) {
  if (id == "skyrimse") return rx::bethesda::Game::kSkyrimSe;
  if (id == "fo4") return rx::bethesda::Game::kFallout4;
  if (id == "fo76") return rx::bethesda::Game::kFallout76;
  if (id == "starfield") return rx::bethesda::Game::kStarfield;
  if (id == "fo3") return rx::bethesda::Game::kFallout3;
  if (id == "fnv") return rx::bethesda::Game::kFalloutNv;
  return rx::bethesda::Game::kUnknown;
}

}  // namespace

int main(int argc, char** argv) {
  rx::EngineConfig config;
  config.headless = true;
  config.host_server = true;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };

    if (arg == "--data-dir") config.data_dir = next();
    else if (arg == "--plugins") config.plugins_txt = next();
    else if (arg == "--game") config.game = ParseGame(next());
    else if (arg == "--port") config.port = static_cast<rx::u16>(std::stoi(next()));
    else if (arg == "--max-clients") config.max_clients = static_cast<rx::u32>(std::stoi(next()));
    else if (arg == "--mods-dir") config.mods_dir = next();
    else {
      PrintUsage();
      return arg == "--help" ? 0 : 1;
    }
  }

  if (config.plugins_txt.empty() && !config.data_dir.empty()) {
    config.plugins_txt = config.data_dir + "/../plugins.txt";
  }

  // Headless app::Host + the game as its Application; the host runs the
  // fixed-step + OnSimulate loop without a renderer.
  rx::app::AppConfig app_config;
  app_config.preset = config.preset;
  app_config.headless = config.headless;
  app_config.gather_entity_draws = false;

  rx::Engine engine(config);
  rx::app::Host host;
  if (!host.Initialize(app_config, engine)) {
    RX_ERROR("server initialization failed");
    return 1;
  }

  g_engine = &engine;
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);
#ifndef _WIN32
  std::signal(SIGHUP, HandleReload);  // kill -HUP <pid> to reload mods live
#endif

  RX_INFO("dedicated server up on port {}, ctrl-c to stop", config.port);
  int rc = host.Run();
  host.Shutdown();
  RX_INFO("server stopped");
  return rc;
}
