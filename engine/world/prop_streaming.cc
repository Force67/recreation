#include "world/prop_streaming.h"

namespace rx::world {

namespace {

constexpr u32 FourCc(char a, char b, char c, char d) {
  return static_cast<u32>(a) | (static_cast<u32>(b) << 8) | (static_cast<u32>(c) << 16) |
         (static_cast<u32>(d) << 24);
}

}  // namespace

PropClassification ClassifyProp(const PropTraits& traits) {
  constexpr u32 kStat = FourCc('S', 'T', 'A', 'T');
  constexpr u32 kTree = FourCc('T', 'R', 'E', 'E');
  constexpr u32 kDoor = FourCc('D', 'O', 'O', 'R');
  constexpr u32 kContainer = FourCc('C', 'O', 'N', 'T');
  constexpr u32 kFurniture = FourCc('F', 'U', 'R', 'N');
  constexpr u32 kActivator = FourCc('A', 'C', 'T', 'I');

  PropClassification out;
  if (traits.placed_script || traits.base_script) out.capabilities |= kPropScripted;
  if (traits.base_type == kDoor) out.capabilities |= kPropDoor | kPropActivatable;
  if (traits.base_type == kContainer) out.capabilities |= kPropContainer | kPropActivatable;
  if (traits.base_type == kFurniture || traits.base_type == kActivator) {
    out.capabilities |= kPropActivatable;
  }

  const bool immutable_type = traits.base_type == kStat || traits.base_type == kTree;
  out.batchable = immutable_type && out.capabilities == kPropNone && !traits.primitive &&
                  !traits.teleport && !traits.stateful;
  return out;
}

u64 PackInChildHandle(u64 parent, u64 source_child) {
  const u64 source =
      parent ^ (source_child + 0x9e3779b97f4a7c15ull + (parent << 6) + (parent >> 2));
  u64 hash = source;
  hash ^= hash >> 30;
  hash *= 0xbf58476d1ce4e5b9ull;
  hash ^= hash >> 27;
  hash *= 0x94d049bb133111ebull;
  hash ^= hash >> 31;
  const u16 plugin = static_cast<u16>(0x8000u | ((hash >> 32) & 0x7ffeu));
  return static_cast<u64>(plugin) << 32 | static_cast<u32>(hash);
}

}  // namespace rx::world
