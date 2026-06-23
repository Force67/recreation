#include "modstream/content_provider.h"

#include <filesystem>
#include <fstream>
#include <unordered_map>

#include "modstream/mod_catalog.h"

namespace rec::modstream {
namespace {

namespace fs = std::filesystem;

std::optional<base::Vector<u8>> ReadFile(const fs::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) return std::nullopt;
  base::Vector<u8> data(static_cast<size_t>(file.tellg()));
  file.seekg(0);
  file.read(reinterpret_cast<char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
  if (!file) return std::nullopt;
  return data;
}

// Serves one resource's files from absolute on-disk paths, resolved when the
// resource is mounted. Backs both the client (cache files) and the host (the
// original mods directory), so the two mount identical content the same way.
class ResourceProvider final : public asset::FileProvider {
 public:
  ResourceProvider(std::string name, std::unordered_map<std::string, fs::path> paths)
      : name_(std::move(name)), paths_(std::move(paths)) {}

  bool Contains(std::string_view normalized_path) const override {
    return paths_.find(std::string(normalized_path)) != paths_.end();
  }

  std::optional<base::Vector<u8>> Read(std::string_view normalized_path) const override {
    const auto it = paths_.find(std::string(normalized_path));
    if (it == paths_.end()) return std::nullopt;
    return ReadFile(it->second);
  }

  void Enumerate(const std::function<void(std::string_view)>& fn) const override {
    for (const auto& [path, disk] : paths_) fn(path);
  }

  std::string name() const override { return name_; }

 private:
  std::string name_;
  std::unordered_map<std::string, fs::path> paths_;
};

}  // namespace

void MountManifest(asset::Vfs& vfs, const ModManifest& manifest,
                   const ContentStore& store) {
  for (const ModResource& resource : manifest.resources) {
    std::unordered_map<std::string, fs::path> paths;
    paths.reserve(resource.files.size());
    for (const ResourceFile& file : resource.files) {
      if (std::optional<fs::path> p = store.PathFor(file.hash)) paths.emplace(file.path, *p);
    }
    vfs.Mount(base::MakeUnique<ResourceProvider>("modstream:" + resource.name, std::move(paths)));
  }
}

void MountCatalog(asset::Vfs& vfs, const ModCatalog& catalog) {
  for (const ModResource& resource : catalog.manifest().resources) {
    std::unordered_map<std::string, fs::path> paths;
    paths.reserve(resource.files.size());
    for (const ResourceFile& file : resource.files) {
      if (std::optional<fs::path> p = catalog.PathForHash(file.hash)) paths.emplace(file.path, *p);
    }
    vfs.Mount(base::MakeUnique<ResourceProvider>("modstream:" + resource.name, std::move(paths)));
  }
}

}  // namespace rec::modstream
