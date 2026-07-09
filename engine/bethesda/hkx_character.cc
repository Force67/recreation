#include "bethesda/hkx_character.h"

namespace rx::bethesda {
namespace {

// hkbCharacterStringData, hk2010 x64 serialized image. Verified against
// SE characters/defaultmale.hkx (hkxinfo --hex): the hkArray at +0x30 counts
// 1656 entries whose strings are the animation paths.
constexpr u64 kAnimationNames = 0x30;

}  // namespace

std::vector<std::string> DecodeAnimationNames(const HkxFile& hkx) {
  std::vector<std::string> names;
  for (const HkxObject& object : hkx.objects()) {
    if (object.class_name != "hkbCharacterStringData") continue;
    u32 count = 0;
    u64 base = hkx.Array(object.offset + kAnimationNames, &count);
    if (base == HkxFile::kNull) return names;
    names.reserve(count);
    for (u32 i = 0; i < count; ++i) {
      names.emplace_back(hkx.CString(base + i * 8ull));
    }
    return names;
  }
  return names;
}

}  // namespace rx::bethesda
