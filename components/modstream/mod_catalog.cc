#include "components/modstream/mod_catalog.h"

#include <base/algorithm.h>
#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/memory/move.h>
#include <base/optional.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include <fstream>
#include <sstream>
#include <system_error>

#include "asset/asset_id.h"
#include "components/modstream/content_hash.h"
#include "components/modstream/stream_filter.h"
#include "core/log.h"

namespace rx::modstream {
namespace {

namespace fs = std::filesystem;

// Strips leading and trailing ASCII whitespace; mirrors StreamFilter's trim.
base::StringRef Trim(base::StringRef s) {
  const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
  while (!s.empty() && is_space(s.front()))
    s.remove_prefix(1);
  while (!s.empty() && is_space(s.back()))
    s.remove_suffix(1);
  return s;
}

// Parses a resource's `client_scripts.txt`: one resource-relative assembly path
// per line, blank lines and `#` comments ignored. Returns normalized paths in
// file order; deduplicated and sorted by the caller.
base::Vector<base::String> ParseClientScriptFile(const fs::path& path) {
  base::Vector<base::String> scripts;
  std::ifstream in(path.c_str(), std::ios::binary);
  if (!in)
    return scripts;
  std::ostringstream buffer;
  buffer << in.rdbuf();
  std::istringstream in_text(buffer.str());
  std::string source;
  while (std::getline(in_text, source)) {
    const base::String raw(source.c_str(), source.size());
    const base::StringRef line = Trim(raw);
    if (line.empty() || line.front() == '#')
      continue;
    // Normalize to match catalog paths regardless of the case or slash style
    // the resource author used.
    base::String script = asset::NormalizePath(line);
    if (!script.empty())
      scripts.push_back(base::move(script));
  }
  return scripts;
}

// Builds one ModResource from a resource directory. Returns nullopt if any file
// underneath cannot be hashed, propagating the failure up to Build.
base::Optional<ModResource> ScanResource(const fs::path& root,
                                         const base::String& name,
                                         base::UnorderedMap<ContentHash, fs::path>& by_hash) {
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
  for (fs::recursive_directory_iterator it(root, fs::directory_options::none, ec); !ec && it != end;
       it.increment(ec)) {
    const fs::directory_entry& entry = *it;
    if (!entry.is_regular_file(ec) || ec) {
      if (ec)
        return base::nullopt;
      continue;
    }
    const fs::path rel = fs::relative(entry.path(), root, ec);
    if (ec)
      return base::nullopt;
    const base::String norm = asset::NormalizePath(rel.generic_string());

    // Excluded and server-only files never enter the catalog, so the server
    // cannot serve them and clients never learn they exist. The client-script
    // declaration is a server directive like .streamignore, not content: it
    // reaches clients as its own validated message, never as a file.
    if (norm == ".streamignore" || norm == "client_scripts.txt" || filter.Excludes(norm))
      continue;

    const base::Optional<ContentHash> hash = HashFile(entry.path());
    if (!hash)
      return base::nullopt;

    const u64 file_size = entry.file_size(ec);
    if (ec)
      return base::nullopt;

    ResourceFile file;
    file.path = norm;
    file.size = file_size;
    file.hash = *hash;
    by_hash.emplace(*hash, entry.path());
    resource.files.push_back(base::move(file));
  }
  if (ec)
    return base::nullopt;

  base::Sort(resource.files.begin(), resource.files.end(),
             [](const ResourceFile& a, const ResourceFile& b) { return a.path < b.path; });

  // Resolve the declaration against the catalog: a script that names a missing
  // file (typo) or a streamignored one (server-only) cannot run on clients, so
  // it is dropped here — clients only ever hear about assemblies they can
  // actually receive. Duplicates in the file collapse.
  const base::Vector<base::String> declared =
      ParseClientScriptFile(root / "client_scripts.txt");
  if (!declared.empty()) {
    for (base::String& script : declared) {
      const auto* it = base::FindIf(resource.files.begin(), resource.files.end(),
                                    [&script](const ResourceFile& f) { return f.path == script; });
      if (it == resource.files.end()) {
        RX_WARN("mods: resource '{}' declares client script '{}' that is not in the resource "
                "(missing or .streamignore'd); dropping it",
                resource.name.c_str(), script.c_str());
        continue;
      }
      if (!resource.client_scripts.Contains(script))
        resource.client_scripts.push_back(base::move(script));
    }
    base::Sort(resource.client_scripts.begin(), resource.client_scripts.end(),
               [](const base::String& a, const base::String& b) { return a < b; });
  }
  return resource;
}

}  // namespace

base::Optional<ModCatalog> ModCatalog::Build(const fs::path& mods_dir) {
  std::error_code ec;
  if (!fs::is_directory(mods_dir, ec) || ec)
    return base::nullopt;

  ModCatalog catalog;
  base::Vector<fs::directory_entry> entries;
  for (const fs::directory_entry& entry : fs::directory_iterator(mods_dir.c_str(), ec)) {
    if (entry.is_directory())
      entries.push_back(entry);
  }
  if (ec)
    return base::nullopt;

  // Sort resources by directory name so the manifest is stable across runs.
  base::Sort(entries.begin(), entries.end(),
             [](const fs::directory_entry& a, const fs::directory_entry& b) {
               return a.path().filename() < b.path().filename();
             });

  for (const fs::directory_entry& entry : entries) {
    base::Optional<ModResource> resource =
        ScanResource(entry.path(), entry.path().filename().string(), catalog.by_hash_);
    if (!resource)
      return base::nullopt;
    catalog.manifest_.resources.push_back(base::move(*resource));
  }
  return catalog;
}

base::Optional<fs::path> ModCatalog::PathForHash(ContentHash hash) const {
  const auto* it = by_hash_.find(hash);
  if (it == nullptr)
    return base::nullopt;
  return *it;
}

}  // namespace rx::modstream
