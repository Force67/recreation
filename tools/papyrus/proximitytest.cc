// proximitytest: the record-backed bindings' proximity query over a position
// snapshot. Needs no game data, so it runs in the default ctest gate; it checks
// the distance test and the game->engine unit conversion directly.
#include <array>
#include <cstdio>
#include <utility>
#include <vector>

#include "script/games/skyrim/skyrim_bindings.h"

using rec::script::papyrus::ObjectRef;
using rec::script::skyrim::RecordBackedSkyrimBindings;

int main() {
  RecordBackedSkyrimBindings bindings;  // no records needed for proximity

  // Engine-space positions: the centre at the origin, two close refs and one far.
  std::vector<std::pair<rec::u64, std::array<rec::f32, 3>>> snapshot = {
      {0x14, {0.0f, 0.0f, 0.0f}},     // centre
      {0x100, {3.0f, 0.0f, 0.0f}},    // 3 m away
      {0x101, {0.0f, 4.0f, 0.0f}},    // 4 m away
      {0x102, {1000.0f, 0.0f, 0.0f}}, // far
  };
  bindings.UpdatePositionSnapshot(snapshot);

  int failures = 0;
  auto check = [&](const char* what, bool ok) {
    std::printf("  %-44s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
  };

  // 400 game units * 0.01428 ~= 5.7 m: includes the 3 m and 4 m refs, not the far.
  const int count = bindings.GetNearbyRefs(ObjectRef{0x14}, 400.0f);
  check("two refs within radius", count == 2);

  bool saw_100 = false, saw_101 = false, saw_102 = false, saw_center = false;
  for (int i = 0; i < count; ++i) {
    const rec::u64 h = bindings.GetNthNearbyRef(i).handle;
    const rec::f32 d = bindings.GetNthNearbyDistance(i);
    saw_100 |= h == 0x100;
    saw_101 |= h == 0x101;
    saw_102 |= h == 0x102;
    saw_center |= h == 0x14;
    // Distance is reported in game units: 3 m engine -> ~210 game units.
    if (h == 0x100) check("3 m ref reports ~210 game units", d > 205.0f && d < 215.0f);
  }
  check("includes the close refs", saw_100 && saw_101);
  check("excludes the far ref", !saw_102);
  check("excludes the centre", !saw_center);

  // A tiny radius finds nothing; an unknown centre finds nothing.
  check("tiny radius finds none", bindings.GetNearbyRefs(ObjectRef{0x14}, 1.0f) == 0);
  check("unknown centre finds none", bindings.GetNearbyRefs(ObjectRef{0x999}, 400.0f) == 0);

  std::printf("%s (%d failures)\n", failures ? "PROXIMITYTEST FAILED" : "PROXIMITYTEST PASSED",
              failures);
  return failures ? 1 : 0;
}
