#include <functional>
#include <base/containers/unordered_map.h>
#include <base/functional/function.h>
#include <base/memory/move.h>
#include <base/optional.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include "components/modstream/content_provider.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#include "components/modstream/mod_catalog.h"

namespace rx::modstream {
namespace {

namespace fs = std::filesystem;

base::Optional<base::Vector<u8>> ReadFile(const fs::path& path) {
  std::ifstream file(path.c_str(), std::ios::binary | std::ios::ate);
  if (!file) return base::nullopt;
  base::Vector<u8> data(static_cast<size_t>(file.tellg()));
  file.seekg(0);
  file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
  if (!file) return base::nullopt;
  return data;
}

// Serves one resource's files from absolute on-disk paths, resolved when the
// resource is mounted. Backs both the client (cache files) and the host (the
// original mods directory), so the two mount identical content the same way.
class ResourceProvider final : public asset::FileProvider {
 public:
  ResourceProvider(base::String name, base::UnorderedMap<base::String, fs::path> paths)
      : name_(base::move(name)), paths_(base::move(paths)) {}

  bool Contains(std::string_view normalized_path) const override {
    return paths_.contains(base::String(normalized_path));
  }

  std::optional<base::Vector<u8>> Read(std::string_view normalized_path) const override {
    const auto* it = paths_.find(base::String(normalized_path));
    if (it == nullptr) return std::nullopt;
    base::Optional<base::Vector<u8>> bytes = ReadFile(*it);
    if (!bytes.has_value()) return std::nullopt;
    return base::move(bytes.value());
  }

  void Enumerate(const std::function<void(std::string_view)>& fn) const override {
    for (const auto& [path, disk] : paths_) fn(path);
  }

  std::string name() const override { return name_.c_str(); }

 private:
  base::String name_;
  base::UnorderedMap<base::String, fs::path> paths_;
};

}  // namespace

void MountManifest(asset::Vfs& vfs, const ModManifest& manifest, const ContentStore& store) {
  for (const ModResource& resource : manifest.resources) {
    base::UnorderedMap<base::String, fs::path> paths;
    paths.reserve(resource.files.size());
    for (const ResourceFile& file : resource.files) {
      if (base::Optional<fs::path> p = store.PathFor(file.hash)) paths.emplace(file.path, *p);
    }
    vfs.Mount(base::MakeUnique<ResourceProvider>("modstream:" + resource.name, base::move(paths)));
  }
}

void MountCatalog(asset::Vfs& vfs, const ModCatalog& catalog) {
  for (const ModResource& resource : catalog.manifest().resources) {
    base::UnorderedMap<base::String, fs::path> paths;
    paths.reserve(resource.files.size());
    for (const ResourceFile& file : resource.files) {
      if (base::Optional<fs::path> p = catalog.PathForHash(file.hash)) paths.emplace(file.path, *p);
    }
    vfs.Mount(base::MakeUnique<ResourceProvider>("modstream:" + resource.name, base::move(paths)));
  }
}

}  // namespace rx::modstream
