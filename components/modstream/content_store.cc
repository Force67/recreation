#include "components/modstream/content_store.h"

#include <base/containers/vector.h>
#include <base/memory/move.h>
#include <base/optional.h>
#include <base/strings/xstring.h>

#include <cstdio>
#include <fstream>
#include <system_error>

#include "components/modstream/content_hash.h"

namespace rx::modstream {
namespace {

namespace fs = std::filesystem;

// 16-char lowercase hex of a 64-bit hash, the cache file's stable name.
base::String HexName(ContentHash hash) {
  static constexpr char kDigits[] = "0123456789abcdef";
  base::String s(16, '0');
  for (int i = 15; i >= 0; --i) {
    s[i] = kDigits[hash & 0xf];
    hash >>= 4;
  }
  return s;
}

// First hex digit, used as the shard subdirectory so one cache does not pile
// every file into a single directory.
base::String ShardName(ContentHash hash) {
  static constexpr char kDigits[] = "0123456789abcdef";
  return base::String(1, kDigits[(hash >> 60) & 0xf]);
}

}  // namespace

ContentStore::ContentStore(fs::path root) : root_(base::move(root)) {}

fs::path ContentStore::PathOf(ContentHash hash) const {
  return root_ / ShardName(hash).c_str() / (HexName(hash) + ".bin").c_str();
}

bool ContentStore::EnsureShard(ContentHash hash) const {
  std::error_code ec;
  fs::create_directories(root_ / ShardName(hash).c_str(), ec);
  return !ec;
}

bool ContentStore::Has(ContentHash hash) const {
  std::error_code ec;
  return fs::is_regular_file(PathOf(hash), ec) && !ec;
}

base::Optional<fs::path> ContentStore::PathFor(ContentHash hash) const {
  const fs::path path = PathOf(hash);
  std::error_code ec;
  if (fs::is_regular_file(path, ec) && !ec) return path;
  return base::nullopt;
}

base::Optional<fs::path> ContentStore::Store(ContentHash expected, const base::Vector<u8>& bytes) {
  if (HashBytes(bytes.data(), bytes.size()) != expected) return base::nullopt;
  if (!EnsureShard(expected)) return base::nullopt;

  const fs::path final_path = PathOf(expected);
  const fs::path temp_path = final_path.string() + ".part";
  {
    std::ofstream out(temp_path.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) return base::nullopt;
    if (!bytes.empty()) {
      out.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    out.flush();
    if (!out) return base::nullopt;
  }

  std::error_code ec;
  fs::rename(temp_path, final_path, ec);
  if (ec) {
    fs::remove(temp_path.c_str(), ec);
    return base::nullopt;
  }
  return final_path;
}

base::Optional<fs::path> ContentStore::Adopt(ContentHash expected, const fs::path& source) {
  const base::Optional<ContentHash> actual = HashFile(source);
  std::error_code ec;
  if (!actual || *actual != expected) {
    fs::remove(source.c_str(), ec);
    return base::nullopt;
  }
  if (!EnsureShard(expected)) return base::nullopt;

  const fs::path final_path = PathOf(expected);
  fs::rename(source, final_path, ec);
  if (ec) {
    // A cross-device move (temp on another filesystem) cannot rename; fall back
    // to a copy, then drop the source. This is a real move, not a integrity
    // shortcut: the bytes were already verified above.
    fs::copy_file(source, final_path, fs::copy_options::overwrite_existing, ec);
    std::error_code rm_ec;
    fs::remove(source.c_str(), rm_ec);
    if (ec) return base::nullopt;
  }
  return final_path;
}

base::Optional<ContentHash> ContentStore::Ingest(const fs::path& source) {
  const base::Optional<ContentHash> hash = HashFile(source);
  if (!hash) return base::nullopt;

  std::error_code ec;
  if (Has(*hash)) {
    // Identical content already cached; drop the redundant copy.
    fs::remove(source.c_str(), ec);
    return hash;
  }
  if (!EnsureShard(*hash)) return base::nullopt;

  const fs::path final_path = PathOf(*hash);
  fs::rename(source, final_path, ec);
  if (ec) {
    fs::copy_file(source, final_path, fs::copy_options::overwrite_existing, ec);
    std::error_code rm_ec;
    fs::remove(source.c_str(), rm_ec);
    if (ec) return base::nullopt;
  }
  return hash;
}

}  // namespace rx::modstream
