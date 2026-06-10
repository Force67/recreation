#include <cstring>
#include <string>

#include "engine.h"
#include "core/log.h"

namespace {

void PrintUsage() {
  REC_INFO("usage: recreation --data-dir <path> [options]");
  REC_INFO("  --data-dir <path>     game Data directory");
  REC_INFO("  --plugins <path>      plugins.txt (default: <data-dir>/../plugins.txt)");
  REC_INFO("  --game <id>           skyrimse | fo4 | fo76 (default: autodetect)");
  REC_INFO("  --headless            no window, no renderer");
  REC_INFO("  --server              host a server");
  REC_INFO("  --connect <address>   join a server");
  REC_INFO("  --port <port>         server port (default: 29700)");
  REC_INFO("  --no-taa              disable temporal antialiasing");
  REC_INFO("  --upscaler <id>       fsr3 | dlss | xess");
  REC_INFO("  --no-rt               disable raytracing");
}

rec::bethesda::Game ParseGame(const std::string& id) {
  if (id == "skyrimse") return rec::bethesda::Game::kSkyrimSe;
  if (id == "fo4") return rec::bethesda::Game::kFallout4;
  if (id == "fo76") return rec::bethesda::Game::kFallout76;
  return rec::bethesda::Game::kUnknown;
}

rec::render::UpscalerKind ParseUpscaler(const std::string& id) {
  if (id == "fsr3") return rec::render::UpscalerKind::kFsr3;
  if (id == "dlss") return rec::render::UpscalerKind::kDlss;
  if (id == "xess") return rec::render::UpscalerKind::kXess;
  return rec::render::UpscalerKind::kNone;
}

}  // namespace

int main(int argc, char** argv) {
  rec::EngineConfig config;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };

    if (arg == "--data-dir") config.data_dir = next();
    else if (arg == "--plugins") config.plugins_txt = next();
    else if (arg == "--game") config.game = ParseGame(next());
    else if (arg == "--headless") config.headless = true;
    else if (arg == "--server") config.host_server = true;
    else if (arg == "--connect") config.connect_address = next();
    else if (arg == "--port") config.port = static_cast<rec::u16>(std::stoi(next()));
    else if (arg == "--no-taa") config.renderer.aa_mode = rec::render::AntiAliasingMode::kNone;
    else if (arg == "--upscaler") config.renderer.upscaler = ParseUpscaler(next());
    else if (arg == "--no-rt") config.renderer.enable_raytracing = false;
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
    REC_ERROR("engine initialization failed");
    return 1;
  }
  int rc = engine.Run();
  engine.Shutdown();
  return rc;
}
