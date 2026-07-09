#include "modstream/mod_catalog.h"

#include <algorithm>
#include <system_error>

#include "asset/asset_id.h"
#include "modstream/content_hash.h"
#include "modstream/stream_filter.h"

namespace rx::modstream {
namespace {

namespace fs = std::filesystem;

// Builds one ModResource from a resource directory. Returns nullopt if any file
// underneath cannot be hashed, propagating the failure up to Build.
std::optional<ModResource> ScanResource(
    const fs::path& root, const std::string& name,
    std::unordered_map<ContentHash, fs::path>& by_hash) {
  ModResource resource;
  resource.name = name;

  // Files the resource keeps off clients (server configs, source, secrets). The
  // .streamignore itself never streams. Absent file means everything streams.
  const StreamFilter filter = StreamFilter::FromFile(root / ".streamignore");

  std::error_code ec;
  const fs::recursive_directory_iterator end;
  // Drive the walk with the error-code overloads so a mid-traversal failure
  // (a permission change, a race) returns nullopt like every other error here,
  // rather than throwing out of Build.
  for (fs::recursive_directory_iterator it(root, fs::directory_options::none, ec);
       !ec && it != end; it.increment(ec)) {
    const fs::directory_entry& entry = *it;
    if (!entry.is_regular_file(ec) || ec) {
      if (ec) return std::nullopt;
      continue;
    }
    const fs::path rel = fs::relative(entry.path(), root, ec);
    if (ec) return std::nullopt;
    const std::string norm = asset::NormalizePath(rel.generic_string());

    // Excluded and server-only files never enter the catalog, so the server
    // cannot serve them and clients never learn they exist.
    if (norm == ".streamignore" || filter.Excludes(norm)) continue;

    const std::optional<ContentHash> hash = HashFile(entry.path());
    if (!hash) return std::nullopt;

    const u64 file_size = entry.file_size(ec);
    if (ec) return std::nullopt;

    ResourceFile file;
    file.path = norm;
    file.size = file_size;
    file.hash = *hash;
    by_hash.emplace(*hash, entry.path());
    resource.files.push_back(std::move(file));
  }
  if (ec) return std::nullopt;

  std::sort(resource.files.begin(), resource.files.end(),
            [](const ResourceFile& a, const ResourceFile& b) {
              return a.path < b.path;
            });
  return resource;
}

}  // namespace

std::optional<ModCatalog> ModCatalog::Build(const fs::path& mods_dir) {
  std::error_code ec;
  if (!fs::is_directory(mods_dir, ec) || ec) return std::nullopt;

  ModCatalog catalog;
  std::vector<fs::directory_entry> entries;
  for (const fs::directory_entry& entry : fs::directory_iterator(mods_dir, ec)) {
    if (entry.is_directory()) entries.push_back(entry);
  }
  if (ec) return std::nullopt;

  // Sort resources by directory name so the manifest is stable across runs.
  std::sort(entries.begin(), entries.end(),
            [](const fs::directory_entry& a, const fs::directory_entry& b) {
              return a.path().filename() < b.path().filename();
            });

  for (const fs::directory_entry& entry : entries) {
    std::optional<ModResource> resource = ScanResource(
        entry.path(), entry.path().filename().string(), catalog.by_hash_);
    if (!resource) return std::nullopt;
    catalog.manifest_.resources.push_back(std::move(*resource));
  }
  return catalog;
}

std::optional<fs::path> ModCatalog::PathForHash(ContentHash hash) const {
  const auto it = by_hash_.find(hash);
  if (it == by_hash_.end()) return std::nullopt;
  return it->second;
}

}  // namespace rx::modstream
