#include "modstream/stream_filter.h"

#include <fstream>
#include <sstream>

#include "asset/asset_id.h"

namespace rx::modstream {
namespace {

// Strips leading and trailing ASCII whitespace.
std::string_view Trim(std::string_view s) {
  const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
  while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
  while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
  return s;
}

}  // namespace

StreamFilter StreamFilter::Parse(std::string_view text) {
  StreamFilter filter;
  std::istringstream in{std::string(text)};
  std::string raw;
  while (std::getline(in, raw)) {
    const std::string_view line = Trim(raw);
    if (line.empty() || line.front() == '#') continue;
    // Normalize so patterns match the catalog's normalized paths regardless of
    // the case or slash style the author used.
    std::string pattern = asset::NormalizePath(line);
    if (pattern.empty()) continue;
    if (pattern.back() == '/') {
      filter.dir_prefixes_.push_back(std::move(pattern));
    } else if (pattern.size() > 2 && pattern[0] == '*' && pattern[1] == '.') {
      filter.ext_suffixes_.push_back(pattern.substr(1));  // ".bak"
    } else {
      filter.exact_paths_.push_back(std::move(pattern));
    }
  }
  return filter;
}

StreamFilter StreamFilter::FromFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return Parse(buffer.str());
}

bool StreamFilter::Excludes(std::string_view path) const {
  for (const std::string& exact : exact_paths_) {
    if (path == exact) return true;
  }
  for (const std::string& prefix : dir_prefixes_) {
    if (path.size() >= prefix.size() &&
        path.compare(0, prefix.size(), prefix) == 0) {
      return true;
    }
  }
  for (const std::string& suffix : ext_suffixes_) {
    if (path.size() >= suffix.size() &&
        path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace rx::modstream
