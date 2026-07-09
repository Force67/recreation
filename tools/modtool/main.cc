// modtool: an offline helper for authors of streamed mods. `inspect` catalogs a
// mods directory exactly as the server would and prints what each client will
// receive, so an author can confirm their layout is valid and that a
// .streamignore is keeping the right files server-side before deploying. It is
// pure tooling over the modstream catalog; it opens no socket.

#include <cstdio>
#include <cstring>
#include <string>

#include "modstream/mod_catalog.h"

namespace {

void PrintUsage() {
  std::printf("usage: modtool inspect <mods-dir>\n");
  std::printf("  Catalogs the mods directory and lists what would be streamed to\n");
  std::printf("  clients. Files excluded by a resource's .streamignore do not appear.\n");
}

int Inspect(const std::string& dir) {
  std::optional<rx::modstream::ModCatalog> catalog = rx::modstream::ModCatalog::Build(dir);
  if (!catalog) {
    std::printf("modtool: cannot catalog '%s' (missing directory or an unreadable file)\n",
                dir.c_str());
    return 1;
  }

  const rx::modstream::ModManifest& manifest = catalog->manifest();
  std::printf("modtool: %s\n", dir.c_str());
  for (const rx::modstream::ModResource& resource : manifest.resources) {
    rx::u64 resource_bytes = 0;
    for (const rx::modstream::ResourceFile& f : resource.files) resource_bytes += f.size;
    std::printf("  resource \"%s\" (%zu files, %llu bytes)\n", resource.name.c_str(),
                resource.files.size(), static_cast<unsigned long long>(resource_bytes));
    for (const rx::modstream::ResourceFile& f : resource.files) {
      std::printf("    %-48s %10llu  %016llx\n", f.path.c_str(),
                  static_cast<unsigned long long>(f.size),
                  static_cast<unsigned long long>(f.hash));
    }
  }
  std::printf("  ---\n  %zu resources, %zu files, %llu bytes streamed to each client\n",
              manifest.resources.size(), manifest.TotalFiles(),
              static_cast<unsigned long long>(manifest.TotalBytes()));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::strcmp(argv[1], "inspect") == 0) return Inspect(argv[2]);
  PrintUsage();
  return argc == 2 && std::strcmp(argv[1], "--help") == 0 ? 0 : 1;
}
