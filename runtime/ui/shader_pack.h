#ifndef RECREATION_RUNTIME_UI_SHADER_PACK_H_
#define RECREATION_RUNTIME_UI_SHADER_PACK_H_

#include <base/containers/vector.h>
#include <base/strings/string_ref.h>

#include <cstddef>

#include "asset/vfs.h"
#include "core/types.h"

// Recreation ships its own compiled shaders (the thumbnailer's; the HUD's ugui
// pipelines come from rx::ui) inside its game archive, Data/recreation.rxp (see
// cmake/shaders.cmake), which the host mounts at recreation://. Pipeline
// creation pulls each blob from recreation://shaders/<stem>.spv instead of the
// binary. The same blobs stay embedded as C arrays and are handed back verbatim
// whenever the archive is missing or lacks the entry, so a client with no
// archive beside it still runs. A loose mount over recreation://shaders/ (later
// mounts win) lets a developer drop a freshly compiled .spv in to override it.

namespace rx::shaderpack {

// Point the loader at the engine Vfs the game archive was mounted into. Call
// once during engine init, before any pipeline is built. Passing null (or never
// calling this) leaves every Load() on its embedded fallback.
void SetVfs(asset::Vfs* vfs);

// Load a recreation-owned shader blob. `stem` is the source name without the
// .hlsl extension and stage suffix intact, e.g. "thumb.vs"; the loader
// resolves recreation://shaders/<stem>.spv. On any miss it returns a copy of the
// embedded fallback bytes, so the result is always the correct blob for the
// shader.
base::Vector<u8> Load(base::StringRef stem, const void* fallback, size_t fallback_size);

}  // namespace rx::shaderpack

#endif  // RECREATION_RUNTIME_UI_SHADER_PACK_H_
