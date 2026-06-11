// Dedicated server runner: headless engine, fixed tick, no renderer. The
// world simulates exactly like a listen server, clients receive it through
// snapshots.

#include <csignal>
#include <cstring>
#include <string>

#include "core/log.h"
#include "engine.h"

namespace {

rec::Engine* g_engine = nullptr;

void HandleSignal(int) {
  if (g_engine) g_engine->RequestQuit();
}

void PrintUsage() {
  REC_INFO("usage: recreation-server [options]");
  REC_INFO("  --data-dir <path>     game Data directory (omit for demo scene)");
  REC_INFO("  --plugins <path>      plugins.txt (default: <data-dir>/../plugins.txt)");
  REC_INFO("  --game <id>           skyrimse | fo4 | fo76 (default: autodetect)");
  REC_INFO("  --port <port>         listen port (default: 29700)");
  REC_INFO("  --max-clients <n>     player slots (default: 64)");
}

rec::bethesda::Game ParseGame(const std::string& id) {
  if (id == "skyrimse") return rec::bethesda::Game::kSkyrimSe;
  if (id == "fo4") return rec::bethesda::Game::kFallout4;
  if (id == "fo76") return rec::bethesda::Game::kFallout76;
  return rec::bethesda::Game::kUnknown;
}

}  // namespace

int main(int argc, char** argv) {
  rec::EngineConfig config;
  config.headless = true;
  config.host_server = true;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };

    if (arg == "--data-dir") config.data_dir = next();
    else if (arg == "--plugins") config.plugins_txt = next();
    else if (arg == "--game") config.game = ParseGame(next());
    else if (arg == "--port") config.port = static_cast<rec::u16>(std::stoi(next()));
    else if (arg == "--max-clients") config.max_clients = static_cast<rec::u32>(std::stoi(next()));
    else {
      PrintUsage();
      return arg == "--help" ? 0 : 1;
    }
  }

  if (config.plugins_txt.empty() && !config.data_dir.empty()) {
    config.plugins_txt = config.data_dir + "/../plugins.txt";
  }

  rec::Engine engine;
  if (!engine.Initialize(config)) {
    REC_ERROR("server initialization failed");
    return 1;
  }

  g_engine = &engine;
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  REC_INFO("dedicated server up on port {}, ctrl-c to stop", config.port);
  int rc = engine.Run();
  engine.Shutdown();
  REC_INFO("server stopped");
  return rc;
}
