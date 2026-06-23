#include "modstream/content_store.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <system_error>

#include "modstream/content_hash.h"

namespace rec::modstream {
namespace {

namespace fs = std::filesystem;

// 16-char lowercase hex of a 64-bit hash, the cache file's stable name.
std::string HexName(ContentHash hash) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string s(16, '0');
  for (int i = 15; i >= 0; --i) {
    s[i] = kDigits[hash & 0xf];
    hash >>= 4;
  }
  return s;
}

// First hex digit, used as the shard subdirectory so one cache does not pile
// every file into a single directory.
std::string ShardName(ContentHash hash) {
  static constexpr char kDigits[] = "0123456789abcdef";
  return std::string(1, kDigits[(hash >> 60) & 0xf]);
}

}  // namespace

ContentStore::ContentStore(fs::path root) : root_(std::move(root)) {}

fs::path ContentStore::PathOf(ContentHash hash) const {
  return root_ / ShardName(hash) / (HexName(hash) + ".bin");
}

bool ContentStore::EnsureShard(ContentHash hash) const {
  std::error_code ec;
  fs::create_directories(root_ / ShardName(hash), ec);
  return !ec;
}

bool ContentStore::Has(ContentHash hash) const {
  std::error_code ec;
  return fs::is_regular_file(PathOf(hash), ec) && !ec;
}

std::optional<fs::path> ContentStore::PathFor(ContentHash hash) const {
  const fs::path path = PathOf(hash);
  std::error_code ec;
  if (fs::is_regular_file(path, ec) && !ec) return path;
  return std::nullopt;
}

std::optional<fs::path> ContentStore::Store(ContentHash expected,
                                            const std::vector<u8>& bytes) {
  if (HashBytes(bytes.data(), bytes.size()) != expected) return std::nullopt;
  if (!EnsureShard(expected)) return std::nullopt;

  const fs::path final_path = PathOf(expected);
  const fs::path temp_path = final_path.string() + ".part";
  {
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) return std::nullopt;
    if (!bytes.empty()) {
      out.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    out.flush();
    if (!out) return std::nullopt;
  }

  std::error_code ec;
  fs::rename(temp_path, final_path, ec);
  if (ec) {
    fs::remove(temp_path, ec);
    return std::nullopt;
  }
  return final_path;
}

std::optional<fs::path> ContentStore::Adopt(ContentHash expected,
                                            const fs::path& source) {
  const std::optional<ContentHash> actual = HashFile(source);
  std::error_code ec;
  if (!actual || *actual != expected) {
    fs::remove(source, ec);
    return std::nullopt;
  }
  if (!EnsureShard(expected)) return std::nullopt;

  const fs::path final_path = PathOf(expected);
  fs::rename(source, final_path, ec);
  if (ec) {
    // A cross-device move (temp on another filesystem) cannot rename; fall back
    // to a copy, then drop the source. This is a real move, not a integrity
    // shortcut: the bytes were already verified above.
    fs::copy_file(source, final_path, fs::copy_options::overwrite_existing, ec);
    std::error_code rm_ec;
    fs::remove(source, rm_ec);
    if (ec) return std::nullopt;
  }
  return final_path;
}

std::optional<ContentHash> ContentStore::Ingest(const fs::path& source) {
  const std::optional<ContentHash> hash = HashFile(source);
  if (!hash) return std::nullopt;

  std::error_code ec;
  if (Has(*hash)) {
    // Identical content already cached; drop the redundant copy.
    fs::remove(source, ec);
    return hash;
  }
  if (!EnsureShard(*hash)) return std::nullopt;

  const fs::path final_path = PathOf(*hash);
  fs::rename(source, final_path, ec);
  if (ec) {
    fs::copy_file(source, final_path, fs::copy_options::overwrite_existing, ec);
    std::error_code rm_ec;
    fs::remove(source, rm_ec);
    if (ec) return std::nullopt;
  }
  return hash;
}

}  // namespace rec::modstream
