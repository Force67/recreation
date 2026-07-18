#ifndef RECREATION_RUNTIME_SHADER_PACK_H_
#define RECREATION_RUNTIME_SHADER_PACK_H_

#include <cstddef>
#include <string_view>

#include <base/containers/vector.h>

#include "asset/vfs.h"
#include "core/types.h"

// Recreation ships its own compiled HUD/thumbnail shaders inside the
// shaders.rxp game archive (see cmake/shaders.cmake). At startup the engine
// mounts that archive under the "shaders" scheme and points this loader at the
// Vfs; pipeline creation then pulls each blob from shaders://<stem>.spv instead
// of the binary. The same blobs stay embedded as C arrays and are handed back
// verbatim whenever the archive is missing or lacks the entry, so a client with
// no shaders.rxp beside it still runs. A loose shaders:// mount (later mounts
// win) lets a developer drop a freshly compiled .spv in to override the pack.

namespace rx::shaderpack {

// Point the loader at the engine Vfs the shader archive was mounted into. Call
// once during engine init, before any pipeline is built. Passing null (or never
// calling this) leaves every Load() on its embedded fallback.
void SetVfs(asset::Vfs* vfs);

// Load a recreation-owned shader blob. `stem` is the source name without the
// .hlsl extension and stage suffix intact, e.g. "ugui_quad.vs"; the loader
// resolves shaders://<stem>.spv. On any miss it returns a copy of the embedded
// fallback bytes, so the result is always the correct blob for the shader.
base::Vector<u8> Load(std::string_view stem, const void* fallback,
                      size_t fallback_size);

}  // namespace rx::shaderpack

#endif  // RECREATION_RUNTIME_SHADER_PACK_H_
