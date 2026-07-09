// alias_handletest: the packed alias-handle encoding round-trips and never
// collides with a real form handle or the player. Pure header math, no data.

#include <cstdio>

#include "core/types.h"
#include "script/papyrus/alias_handle.h"

namespace {

using rx::script::papyrus::AliasHandleAliasId;
using rx::script::papyrus::AliasHandleQuest;
using rx::script::papyrus::EncodeAliasHandle;
using rx::script::papyrus::IsAliasHandle;
using rx::script::papyrus::kAliasHandleTag;

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

void RoundTrip(rx::u64 quest, rx::u32 alias) {
  const rx::u64 h = EncodeAliasHandle(quest, alias);
  Check("tagged as an alias handle", IsAliasHandle(h));
  Check("quest survives the round trip", AliasHandleQuest(h) == quest);
  Check("alias id survives the round trip", AliasHandleAliasId(h) == alias);
}

}  // namespace

int main() {
  std::puts("alias handle encoding:");
  // Skyrim.esm quest (plugin 0): handle is just the local id. MQ101 is 0x3372b.
  RoundTrip(0x0003372bull, 0);
  RoundTrip(0x0003372bull, 1);
  RoundTrip(0x0003372bull, 117);  // MQ101 uses three-digit alias ids
  // A high-plugin quest: plugin 0x42 << 32 | local, near the 48-bit form ceiling.
  RoundTrip(0x0042abcdef01ull, 4095);  // max 12-bit alias id

  // Real form handles (packed GlobalFormId, <= 48 bits) and the player (0x14)
  // must never read as alias handles.
  Check("player is not an alias", !IsAliasHandle(0x14ull));
  Check("plugin-0 form is not an alias", !IsAliasHandle(0x0003372bull));
  Check("max form handle is not an alias", !IsAliasHandle(0x0000ffffffffffffull));
  Check("an alias handle is distinct from its form", EncodeAliasHandle(0x3372b, 1) != 0x3372bull);
  Check("the tag bit sits above the 48-bit form range", kAliasHandleTag > 0x0000ffffffffffffull);

  if (g_failures == 0) {
    std::puts("alias_handle: all checks passed");
    return 0;
  }
  std::printf("alias_handle: %d checks FAILED\n", g_failures);
  return 1;
}
