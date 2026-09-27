#include "runtime/app/content.h"

#include <base/memory/move.h>

#include "asset/content_mounts.h"
#include "asset/vfs.h"
#include "core/file_system.h"

namespace rx {

asset::Vfs& Content() {
  static asset::Vfs* vfs = [] {
    auto* v = new asset::Vfs;  // lives as long as the process
    asset::MountContent(*v, kContentName);
    return v;
  }();
  return *vfs;
}

bool ReadContent(base::StringRef path, base::Vector<u8>& out) {
  if (asset::SplitVirtualPath(path).mount.empty())
    return fs::ReadFile(path, &out);
  base::Optional<base::Vector<u8>> bytes = Content().Read(path);
  if (!bytes)
    return false;
  out = base::move(*bytes);
  return true;
}

}  // namespace rx
