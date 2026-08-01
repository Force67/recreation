#ifndef RECREATION_SCRIPT_PAPYRUS_ALIAS_HANDLE_H_
#define RECREATION_SCRIPT_PAPYRUS_ALIAS_HANDLE_H_

#include "core/types.h"

namespace rx::script::papyrus {

// A quest alias addressed as an ObjectRef handle. A real form handle is a packed
// GlobalFormId (plugin << 32 | local, at most 48 bits); an alias handle sets a
// high tag bit and packs the owning quest handle plus the alias id, so it never
// collides with a form ref or with the player (0x14). This lets a quest's alias
// property hold a value the VM can call ReferenceAlias methods on (GetReference,
// GetActorRef, ...), which the bindings resolve back to the alias's filled ref.
constexpr u64 kAliasHandleTag = 1ull << 62;

constexpr u64 EncodeAliasHandle(u64 quest, u32 alias_id) {
  return kAliasHandleTag | (quest << 12) | (static_cast<u64>(alias_id) & 0xfffu);
}

constexpr bool IsAliasHandle(u64 handle) { return (handle & kAliasHandleTag) != 0; }

constexpr u64 AliasHandleQuest(u64 handle) { return (handle & (kAliasHandleTag - 1)) >> 12; }

constexpr u32 AliasHandleAliasId(u64 handle) { return static_cast<u32>(handle & 0xfffu); }

}  // namespace rx::script::papyrus

#endif  // RECREATION_SCRIPT_PAPYRUS_ALIAS_HANDLE_H_
