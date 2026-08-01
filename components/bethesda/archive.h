#ifndef RECREATION_BETHESDA_ARCHIVE_H_
#define RECREATION_BETHESDA_ARCHIVE_H_

#include <base/memory/unique_pointer.h>
#include <base/strings/xstring.h>

#include "asset/vfs.h"
#include "core/types.h"

namespace rx::bethesda {

// BSA: Skyrim SE uses version 105 (104 is original Skyrim, still readable).
// BA2: Fallout 4 uses v1/v7/v8, Fallout 76 adds v2/v3, with GNRL (general)
// and DX10 (texture) variants. Both open as a FileProvider so the Vfs treats
// archives and loose directories uniformly.
base::UniquePointer<asset::FileProvider> OpenBsa(const base::String& path);
base::UniquePointer<asset::FileProvider> OpenBa2(const base::String& path);

// Dispatches on extension and magic.
base::UniquePointer<asset::FileProvider> OpenArchive(const base::String& path);

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_ARCHIVE_H_
