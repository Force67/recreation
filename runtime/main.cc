#include <cstring>
#include <string>

#include "engine.h"
#include "core/log.h"

namespace {

void PrintUsage() {
  RX_INFO("usage: recreation --data-dir <path> [options]");
  RX_INFO("  --data-dir <path>     game Data directory");
  RX_INFO("  --plugins <path>      plugins.txt (default: <data-dir>/../plugins.txt)");
  RX_INFO("  --gltf <path>         load a gltf/glb scene (e.g. assets/sponza/Sponza.gltf)");
  RX_INFO("  --menu                open the NEXUS main menu (pick a universe to play)");
  RX_INFO("  --demo <id>           builtin scene: water | materials | gaussian");
  RX_INFO("  --game <id>           skyrimse | fo4 | fo76 (default: autodetect)");
  RX_INFO("  --add-game <spec>     load another game's content live alongside the");
  RX_INFO("                        primary, as <game>:<data-dir>[:<plugins.txt>]");
  RX_INFO("                        (repeatable; runs its own isolated microvm)");
  RX_INFO("  --headless            no window, no renderer");
  RX_INFO("  --server              host a server");
  RX_INFO("  --connect <address>   join a server");
  RX_INFO("  --port <port>         server port (default: 29700)");
  RX_INFO("  --name <name>         player name sent to the server");
  RX_INFO("  --cell <x,y>          exterior start cell (default: 5,-3 near Whiterun)");
  RX_INFO("  --interior <id>       load one interior cell (editor id or 0x form id)");
  RX_INFO("  --grass-density <f>   grass density multiplier (default: 1.0, 0 disables)");
  RX_INFO("  --max-quests <n>      cap quest scripts attached at load (0 = all, default)");
  RX_INFO("  --preset <tier>       auto (default) | android | steamdeck | low |");
  RX_INFO("                        medium | high | ultra | console");
  RX_INFO("  --no-taa              disable temporal antialiasing");
  RX_INFO("  --upscaler <id>       fsr3 | dlss | xess");
  RX_INFO("  --no-rt               disable raytracing");
  RX_INFO("  --validation          enable vulkan validation layers");
}

rx::bethesda::Game ParseGame(const std::string& id) {
  if (id == "skyrimse") return rx::bethesda::Game::kSkyrimSe;
  if (id == "fo4") return rx::bethesda::Game::kFallout4;
  if (id == "fo76") return rx::bethesda::Game::kFallout76;
  if (id == "starfield") return rx::bethesda::Game::kStarfield;
  return rx::bethesda::Game::kUnknown;
}

rx::render::UpscalerKind ParseUpscaler(const std::string& id) {
  if (id == "fsr3") return rx::render::UpscalerKind::kFsr3;
  if (id == "dlss") return rx::render::UpscalerKind::kDlss;
  if (id == "xess") return rx::render::UpscalerKind::kXess;
  return rx::render::UpscalerKind::kNone;
}

}  // namespace

int main(int argc, char** argv) {
  rx::EngineConfig config;

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
      rx::ExtraDomainConfig domain;
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
    else if (arg == "--menu") config.main_menu = true;
    else if (arg == "--headless") config.headless = true;
    else if (arg == "--server") config.host_server = true;
    else if (arg == "--connect") config.connect_address = next();
    else if (arg == "--port") config.port = static_cast<rx::u16>(std::stoi(next()));
    else if (arg == "--name") config.player_name = next();
    else if (arg == "--mods-dir") config.mods_dir = next();
    else if (arg == "--asset-cache") config.asset_cache_dir = next();
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
    else if (arg == "--preset") config.preset = rx::render::ParsePreset(next());
    else if (arg == "--no-taa") config.renderer.aa_mode = rx::render::AntiAliasingMode::kNone;
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

  rx::Engine engine;
  if (!engine.Initialize(config)) {
    RX_ERROR("engine initialization failed");
    return 1;
  }
  int rc = engine.Run();
  engine.Shutdown();
  return rc;
}
