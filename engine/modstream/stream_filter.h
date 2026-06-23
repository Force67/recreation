#ifndef RECREATION_MODSTREAM_STREAM_FILTER_H_
#define RECREATION_MODSTREAM_STREAM_FILTER_H_

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace rec::modstream {

// A per-resource deny list controlling which files reach clients, so server-only
// configs, source data and secrets can sit in the mods directory without being
// streamed. Parsed from a resource's `.streamignore` (gitignore-flavored: one
// pattern per line, blank lines and `#` comments ignored). Three pattern shapes:
//   server/        a trailing-slash directory prefix: excludes everything under it
//   secrets.cfg    an exact resource-relative path
//   *.bak          a `*.ext` extension match
// Patterns are matched against normalized (lowercase, forward-slash) paths. A
// resource with no `.streamignore` excludes nothing, so every file streams.
class StreamFilter {
 public:
  StreamFilter() = default;

  // Parses the contents of a `.streamignore`.
  static StreamFilter Parse(std::string_view text);

  // Loads and parses `path`, or returns an empty filter if it does not exist.
  static StreamFilter FromFile(const std::filesystem::path& path);

  // True if a file at this normalized resource-relative path must not be streamed.
  bool Excludes(std::string_view normalized_rel_path) const;

  bool empty() const {
    return dir_prefixes_.empty() && exact_paths_.empty() && ext_suffixes_.empty();
  }

 private:
  std::vector<std::string> dir_prefixes_;  // e.g. "server/"
  std::vector<std::string> exact_paths_;   // e.g. "secrets.cfg"
  std::vector<std::string> ext_suffixes_;  // e.g. ".bak"
};

}  // namespace rec::modstream

#endif  // RECREATION_MODSTREAM_STREAM_FILTER_H_
