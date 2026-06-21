#include <cstring>
#include <string>

#include "engine.h"
#include "core/log.h"

namespace {

void PrintUsage() {
  REC_INFO("usage: recreation --data-dir <path> [options]");
  REC_INFO("  --data-dir <path>     game Data directory");
  REC_INFO("  --plugins <path>      plugins.txt (default: <data-dir>/../plugins.txt)");
  REC_INFO("  --gltf <path>         load a gltf/glb scene (e.g. assets/sponza/Sponza.gltf)");
  REC_INFO("  --demo <id>           builtin scene: water | materials | gaussian");
  REC_INFO("  --game <id>           skyrimse | fo4 | fo76 (default: autodetect)");
  REC_INFO("  --add-game <spec>     load another game's content live alongside the");
  REC_INFO("                        primary, as <game>:<data-dir>[:<plugins.txt>]");
  REC_INFO("                        (repeatable; runs its own isolated microvm)");
  REC_INFO("  --headless            no window, no renderer");
  REC_INFO("  --server              host a server");
  REC_INFO("  --connect <address>   join a server");
  REC_INFO("  --port <port>         server port (default: 29700)");
  REC_INFO("  --name <name>         player name sent to the server");
  REC_INFO("  --cell <x,y>          exterior start cell (default: 5,-3 near Whiterun)");
  REC_INFO("  --interior <id>       load one interior cell (editor id or 0x form id)");
  REC_INFO("  --grass-density <f>   grass density multiplier (default: 1.0, 0 disables)");
  REC_INFO("  --max-quests <n>      cap quest scripts attached at load (0 = all, default)");
  REC_INFO("  --preset <tier>       auto (default) | android | steamdeck | low |");
  REC_INFO("                        medium | high | ultra | console");
  REC_INFO("  --no-taa              disable temporal antialiasing");
  REC_INFO("  --upscaler <id>       fsr3 | dlss | xess");
  REC_INFO("  --no-rt               disable raytracing");
  REC_INFO("  --validation          enable vulkan validation layers");
}

rec::bethesda::Game ParseGame(const std::string& id) {
  if (id == "skyrimse") return rec::bethesda::Game::kSkyrimSe;
  if (id == "fo4") return rec::bethesda::Game::kFallout4;
  if (id == "fo76") return rec::bethesda::Game::kFallout76;
  if (id == "starfield") return rec::bethesda::Game::kStarfield;
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
    else if (arg == "--gltf") config.gltf_path = next();
    else if (arg == "--demo") config.demo_scene = next();
    else if (arg == "--plugins") config.plugins_txt = next();
    else if (arg == "--game") config.game = ParseGame(next());
    else if (arg == "--add-game") {
      // <game>:<data-dir>[:<plugins.txt>]; the data dir may itself be absolute
      // (a leading drive-less unix path), so split on the first and last colons.
      std::string spec = next();
      size_t first = spec.find(':');
      if (first == std::string::npos) {
        PrintUsage();
        return 1;
      }
      rec::ExtraDomainConfig domain;
      domain.game = ParseGame(spec.substr(0, first));
      std::string rest = spec.substr(first + 1);
      size_t plugins = rest.rfind(':');
      // A bare "C" style colon inside the path is unlikely on this platform; a
      // trailing ":plugins.txt" is recognized only when it ends in .txt.
      if (plugins != std::string::npos && rest.substr(plugins + 1).ends_with(".txt")) {
        domain.data_dir = rest.substr(0, plugins);
        domain.plugins_txt = rest.substr(plugins + 1);
      } else {
        domain.data_dir = rest;
      }
      if (domain.plugins_txt.empty() && !domain.data_dir.empty()) {
        domain.plugins_txt = domain.data_dir + "/../plugins.txt";
      }
      config.extra_domains.push_back(domain);
    }
    else if (arg == "--headless") config.headless = true;
    else if (arg == "--server") config.host_server = true;
    else if (arg == "--connect") config.connect_address = next();
    else if (arg == "--port") config.port = static_cast<rec::u16>(std::stoi(next()));
    else if (arg == "--name") config.player_name = next();
    else if (arg == "--cell") {
      std::string cell = next();
      size_t comma = cell.find(',');
      if (comma == std::string::npos) {
        PrintUsage();
        return 1;
      }
      config.start_cell_x = std::stoi(cell.substr(0, comma));
      config.start_cell_y = std::stoi(cell.substr(comma + 1));
      config.start_cell_explicit = true;
    }
    else if (arg == "--interior") config.interior = next();
    else if (arg == "--grass-density") config.grass_density = std::stof(next());
    else if (arg == "--max-quests") config.max_quest_scripts = std::stoi(next());
    else if (arg == "--preset") config.preset = rec::render::ParsePreset(next());
    else if (arg == "--no-taa") config.renderer.aa_mode = rec::render::AntiAliasingMode::kNone;
    else if (arg == "--upscaler") config.renderer.upscaler = ParseUpscaler(next());
    else if (arg == "--no-rt") config.renderer.enable_raytracing = false;
    else if (arg == "--validation") config.renderer.enable_validation = true;
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
