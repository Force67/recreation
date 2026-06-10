#include "asset/vfs.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "asset/asset_id.h"

namespace rec::asset {

AssetId MakeAssetId(std::string_view normalized_path) { return AssetId{Fnv1a(normalized_path)}; }

std::string NormalizePath(std::string_view path) {
  std::string out(path);
  std::ranges::transform(out, out.begin(), [](char c) {
    if (c == '\\') return '/';
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  return out;
}

void Vfs::Mount(std::unique_ptr<FileProvider> provider) {
  providers_.push_back(std::move(provider));
}

std::optional<std::vector<u8>> Vfs::Read(std::string_view path) const {
  std::string normalized = NormalizePath(path);
  for (auto it = providers_.rbegin(); it != providers_.rend(); ++it) {
    if ((*it)->Contains(normalized)) return (*it)->Read(normalized);
  }
  return std::nullopt;
}

bool Vfs::Contains(std::string_view path) const {
  std::string normalized = NormalizePath(path);
  return std::ranges::any_of(providers_,
                             [&](const auto& provider) { return provider->Contains(normalized); });
}

namespace {

class LooseFileProvider final : public FileProvider {
 public:
  explicit LooseFileProvider(std::string root) : root_(std::move(root)) {}

  bool Contains(std::string_view normalized_path) const override {
    return std::filesystem::exists(root_ / std::filesystem::path(normalized_path));
  }

  std::optional<std::vector<u8>> Read(std::string_view normalized_path) const override {
    std::ifstream file(root_ / std::filesystem::path(normalized_path),
                       std::ios::binary | std::ios::ate);
    if (!file) return std::nullopt;
    std::vector<u8> data(static_cast<size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return data;
  }

  void Enumerate(const std::function<void(std::string_view)>& fn) const override {
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root_, ec)) {
      if (!entry.is_regular_file()) continue;
      fn(NormalizePath(std::filesystem::relative(entry.path(), root_).generic_string()));
    }
  }

  std::string name() const override { return root_.string(); }

 private:
  std::filesystem::path root_;
};

}  // namespace

std::unique_ptr<FileProvider> MakeLooseFileProvider(std::string root_directory) {
  return std::make_unique<LooseFileProvider>(std::move(root_directory));
}

}  // namespace rec::asset
