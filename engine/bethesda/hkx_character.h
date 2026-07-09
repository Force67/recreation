#ifndef RECREATION_BETHESDA_HKX_CHARACTER_H_
#define RECREATION_BETHESDA_HKX_CHARACTER_H_

// hkbCharacterStringData decoding (the characters/*.hkx project files).
// The animationNames array is the behavior project's animation cache order:
// the clip blocks in meshes/animationdata/<project>.txt reference animations
// by index into this list, which is how root motion and trigger events from
// the boundanims sidecars are keyed back to .hkx files on disk.

#include <string>
#include <vector>

#include "bethesda/hkx.h"

namespace rx::bethesda {

// animationNames from the first hkbCharacterStringData in the file
// ("Animations\MT_WalkForward.hkx", ...). Empty when the file has none.
std::vector<std::string> DecodeAnimationNames(const HkxFile& hkx);

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_HKX_CHARACTER_H_
