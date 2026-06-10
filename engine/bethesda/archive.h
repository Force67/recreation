#ifndef RECREATION_BETHESDA_ARCHIVE_H_
#define RECREATION_BETHESDA_ARCHIVE_H_

#include <memory>
#include <string>

#include "asset/vfs.h"
#include "core/types.h"

namespace rec::bethesda {

// BSA: Skyrim SE uses version 105 (104 is original Skyrim, still readable).
// BA2: Fallout 4 uses v1/v7/v8, Fallout 76 adds v2/v3, with GNRL (general)
// and DX10 (texture) variants. Both open as a FileProvider so the Vfs treats
// archives and loose directories uniformly.
std::unique_ptr<asset::FileProvider> OpenBsa(const std::string& path);
std::unique_ptr<asset::FileProvider> OpenBa2(const std::string& path);

// Dispatches on extension and magic.
std::unique_ptr<asset::FileProvider> OpenArchive(const std::string& path);

}  // namespace rec::bethesda

#endif  // RECREATION_BETHESDA_ARCHIVE_H_
