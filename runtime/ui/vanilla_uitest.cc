// Checks that the screens tools/swfdump translated out of the games' Scaleform
// movies are markup libultragui accepts: every .ugui in the vanilla directory
// is parsed and its widget tree walked.
//
// The translated screens are derived from an installed game, so they are not in
// the repository. With none present the test passes trivially, which is what a
// checkout without game data wants; point it at a directory (argv[1], or
// RX_VANILLA_UI_DIR) after running swfdump to actually exercise it.

#include <ugui/idl/parser.h>
#include <ugui/svg/svg.h>

#include <fstream>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void Check(bool condition, const std::string& what) {
  if (condition)
    return;
  std::printf("FAIL: %s\n", what.c_str());
  ++failures;
}

// A translated screen is one root panel holding the whole display list, so a
// walk that reaches every node also proves the nesting closed correctly.
size_t CountNodes(const ugui::UguiNode& node) {
  size_t total = 1;
  for (const auto& child : node.children)
    total += CountNodes(child);
  return total;
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir;
  if (argc > 1) {
    dir = argv[1];
  } else if (const char* env = std::getenv("RX_VANILLA_UI_DIR")) {
    dir = env;
  } else {
#ifdef RECREATION_VANILLA_UI_DIR_DEFAULT
    dir = RECREATION_VANILLA_UI_DIR_DEFAULT;
#endif
  }

  std::error_code ec;
  if (dir.empty() || !fs::is_directory(dir, ec)) {
    std::printf("vanilla_uitest: no translated screens at '%s', nothing to check\n",
                dir.c_str());
    return 0;
  }

  int screens = 0;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".ugui")
      continue;
    // A leading underscore marks a screen that is not a translated movie (a
    // backdrop dropped in to review the others against, say).
    if (entry.path().filename().string().rfind("_", 0) == 0)
      continue;
    ++screens;
    const std::string path = entry.path().string();

    ugui::UguiDocument doc;
    ugui::Vector<ugui::ParseError> errors;
    const bool parsed = ugui::ParseUguiFile(path.c_str(), doc, errors);
    if (!parsed) {
      for (const auto& error : errors)
        std::printf("  %s:%u: %s\n", path.c_str(), error.line, error.message.c_str());
    }
    Check(parsed, path + " parses");
    Check(doc.roots.size() == 1, path + " has exactly one root panel");
    if (doc.roots.empty())
      continue;

    const size_t nodes = CountNodes(doc.roots[0]);
    Check(nodes > 1, path + " has widgets under its root");

    // Every image the manifest binds has to rasterize: the SVG carries the
    // movie's vector art, and ultragui's own rasterizer is what draws it.
    int assets = 0;
    fs::path manifest_path = entry.path();
    manifest_path.replace_extension(".manifest");
    std::ifstream manifest(manifest_path);
    std::string line;
    while (std::getline(manifest, line)) {
      const size_t tab = line.find('\t');
      if (tab == std::string::npos)
        continue;
      const std::string file = line.substr(tab + 1);
      if (file.size() < 4 || file.compare(file.size() - 4, 4, ".svg") != 0)
        continue;
      const std::string asset = (fs::path(dir) / file).string();
      ugui::SvgImage image;
      Check(ugui::LoadSvg(asset.c_str(), image) && !image.pixels.empty(),
            asset + " rasterizes");
      ++assets;
    }
    std::printf("  %-28s %zu widgets, %d svg\n",
                entry.path().filename().string().c_str(), nodes, assets);
  }

  std::printf("vanilla_uitest: %d screen(s), %d failure(s)\n", screens, failures);
  return failures == 0 ? 0 : 1;
}
