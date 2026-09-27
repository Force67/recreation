#ifndef RECREATION_RUNTIME_APP_CONTENT_H_
#define RECREATION_RUNTIME_APP_CONTENT_H_

#include <base/containers/vector.h>
#include <base/strings/string_ref.h>

#include "core/types.h"

namespace rx::asset {
class Vfs;
}

namespace rx {

// The vfs namespace recreation's own files live under, whichever executable
// runs: Data/recreation.rxp (shaders, ui screens, menu art) and the loose
// config/ beside the binary, as recreation://.
inline constexpr const char* kContentName = "recreation";
// The client's rx app id (desktop and Android): its cache folder.
inline constexpr const char* kAppId = "s1po79kz68qex09l0q3y8hpe";

// recreation's content, mounted the way rx's host mounts it (rx docs/CONFIG.md)
// but reachable from code that holds no host services: the ui loads its
// screens and fonts, the debug ui reads the platform config. Bethesda data is
// not here; it stays a disk path the player points at.
asset::Vfs& Content();

// Reads a whole file: a vfs path ("recreation://ui/art/x.png") through
// Content(), anything else from disk (env overrides, captures, mod manifests).
bool ReadContent(base::StringRef path, base::Vector<u8>& out);

}  // namespace rx

#endif  // RECREATION_RUNTIME_APP_CONTENT_H_
