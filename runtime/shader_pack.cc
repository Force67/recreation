#include "shader_pack.h"

#include <cstring>
#include <string>

#include "core/log.h"

namespace rx::shaderpack {
namespace {
asset::Vfs* g_vfs = nullptr;
}  // namespace

void SetVfs(asset::Vfs* vfs) { g_vfs = vfs; }

base::Vector<u8> Load(std::string_view stem, const void* fallback,
                      size_t fallback_size) {
  if (g_vfs) {
    std::string path = "shaders://";
    path.append(stem);
    path.append(".spv");
    if (auto bytes = g_vfs->Read(path)) {
      RX_TRACE("shader {} loaded from {} ({} bytes)", stem, path, bytes->size());
      return std::move(*bytes);
    }
  }
  // No archive mounted, or the entry is absent: hand back the embedded blob.
  base::Vector<u8> out(fallback_size);
  if (fallback_size) std::memcpy(out.data(), fallback, fallback_size);
  return out;
}

}  // namespace rx::shaderpack
