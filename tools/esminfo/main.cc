#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "bethesda/plugin.h"

// Dumps header info and record counts of a plugin. Handy for checking the
// parser against real game files.
int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: esminfo <plugin.esm> [game: skyrimse|fo4|fo76]\n");
    return 1;
  }

  using namespace rec::bethesda;
  Game game = Game::kSkyrimSe;
  if (argc > 2) {
    std::string id = argv[2];
    if (id == "fo4") game = Game::kFallout4;
    if (id == "fo76") game = Game::kFallout76;
  }

  auto plugin = PluginFile::Open(argv[1], GameProfile::For(game));
  if (!plugin) return 1;

  std::printf("%s\n", plugin->file_name().c_str());
  std::printf("  version: %.2f\n", plugin->version());
  std::printf("  master:  %s\n", plugin->is_master() ? "yes" : "no");
  std::printf("  light:   %s\n", plugin->is_light() ? "yes" : "no");
  std::printf("  records: %u (header)\n", plugin->record_count());
  for (const auto& master : plugin->masters()) {
    std::printf("  requires %s\n", master.c_str());
  }

  std::map<std::string, int> counts;
  plugin->VisitRecords([&](Record& record) {
    char type[5] = {};
    std::memcpy(type, &record.header.type, 4);
    ++counts[type];
  });
  for (const auto& [type, count] : counts) {
    std::printf("  %s x%d\n", type.c_str(), count);
  }
  return 0;
}
