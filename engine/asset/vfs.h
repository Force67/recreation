#ifndef RECREATION_ASSET_VFS_H_
#define RECREATION_ASSET_VFS_H_

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/types.h"

namespace rec::asset {

// A source of files: a loose directory, a BSA, a BA2. Providers are mounted
// into the Vfs in priority order.
class FileProvider {
 public:
  virtual ~FileProvider() = default;

  virtual bool Contains(std::string_view normalized_path) const = 0;
  virtual std::optional<std::vector<u8>> Read(std::string_view normalized_path) const = 0;
  virtual void Enumerate(const std::function<void(std::string_view)>& fn) const = 0;
  virtual std::string name() const = 0;
};

// Later mounts win. Mount base game archives first, then DLC, then mod
// archives in plugin order, then loose files last. This reproduces the
// override behavior mods rely on.
class Vfs {
 public:
  void Mount(std::unique_ptr<FileProvider> provider);

  std::optional<std::vector<u8>> Read(std::string_view path) const;
  bool Contains(std::string_view path) const;

  size_t mount_count() const { return providers_.size(); }

 private:
  std::vector<std::unique_ptr<FileProvider>> providers_;
};

std::unique_ptr<FileProvider> MakeLooseFileProvider(std::string root_directory);

}  // namespace rec::asset

#endif  // RECREATION_ASSET_VFS_H_
