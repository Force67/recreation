#ifndef RECREATION_AUDIO_SOUND_CATALOG_H_
#define RECREATION_AUDIO_SOUND_CATALOG_H_

#include <string>
#include <unordered_map>

#include "bethesda/form_id.h"
#include "core/types.h"

namespace rec::bethesda {
class RecordStore;
}

namespace rec::audio {

// Resolves Bethesda sound forms to playable asset paths. A SOUN (Sound Marker) in
// Skyrim SE / Fallout 4 points at a SNDR (Sound Descriptor) that lists one or
// more files (ANAM); older Skyrim stores the filename directly on the SOUN
// (FNAM). Both layouts are folded into one form-id -> path table, so a caller can
// turn a region's sound reference into a file the Vfs can load.
class SoundCatalog {
 public:
  // Scans every SNDR and SOUN record once and builds the lookup table.
  void Build(const bethesda::RecordStore& records);

  // The asset path (e.g. "sound/fx/...") for a SOUN or SNDR form, or empty when
  // the form is unknown or carries no file.
  std::string PathFor(bethesda::GlobalFormId form) const;

  size_t size() const { return paths_.size(); }

 private:
  std::unordered_map<u64, std::string> paths_;  // form.packed() -> normalized asset path
};

}  // namespace rec::audio

#endif  // RECREATION_AUDIO_SOUND_CATALOG_H_
