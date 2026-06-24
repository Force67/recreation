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

void Vfs::Mount(base::UniquePointer<FileProvider> provider) {
  providers_.push_back(std::move(provider));
}

size_t Vfs::UnmountByPrefix(std::string_view prefix) {
  base::Vector<base::UniquePointer<FileProvider>> kept;
  size_t removed = 0;
  for (size_t i = 0; i < providers_.size(); ++i) {
    const std::string name = providers_[i]->name();
    if (name.size() >= prefix.size() &&
        name.compare(0, prefix.size(), prefix.data(), prefix.size()) == 0) {
      ++removed;
    } else {
      kept.push_back(std::move(providers_[i]));
    }
  }
  providers_ = std::move(kept);
  return removed;
}

std::optional<base::Vector<u8>> Vfs::Read(std::string_view path) const {
  std::string normalized = NormalizePath(path);
  for (size_t i = providers_.size(); i-- > 0;) {
    if (providers_[i]->Contains(normalized)) return providers_[i]->Read(normalized);
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

  std::optional<base::Vector<u8>> Read(std::string_view normalized_path) const override {
    std::ifstream file(root_ / std::filesystem::path(normalized_path),
                       std::ios::binary | std::ios::ate);
    if (!file) return std::nullopt;
    base::Vector<u8> data(static_cast<size_t>(file.tellg()));
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

base::UniquePointer<FileProvider> MakeLooseFileProvider(std::string root_directory) {
  return base::MakeUnique<LooseFileProvider>(std::move(root_directory));
}

}  // namespace rec::asset
