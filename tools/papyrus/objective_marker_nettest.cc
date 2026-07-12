// objective_marker_nettest: deterministic round-trip checks for the active
// objective marker codec. No game data needed, so it runs in ctest.

#include <cstdio>
#include <optional>
#include <vector>

#include "core/types.h"
#include "gamenet/objective_marker_net.h"

// zetanet's headers (pulled in via net/objective_marker_net.h's siblings) inject
// their own scalar aliases, so scalar types stay fully qualified as rx::.
namespace {

using rx::ByteSpan;
using rx::net::DecodeObjectiveMarker;
using rx::net::EncodeObjectiveMarker;
using rx::net::ObjectiveMarkerState;

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

bool Same(const ObjectiveMarkerState& a, const ObjectiveMarkerState& b) {
  return a.active == b.active && a.quest == b.quest && a.x == b.x &&
         a.y == b.y && a.z == b.z;
}

void RoundTrip(const char* what, const ObjectiveMarkerState& m) {
  std::vector<rx::u8> blob = EncodeObjectiveMarker(m);
  Check("encoding is exactly 21 bytes", blob.size() == 21);
  std::optional<ObjectiveMarkerState> decoded = DecodeObjectiveMarker(blob);
  Check(what, decoded.has_value() && Same(m, *decoded));
}

void TestRoundTrip() {
  std::puts("objective marker round trip:");

  RoundTrip("inactive marker survives",
            ObjectiveMarkerState{.active = false, .quest = 0, .x = 0, .y = 0,
                                 .z = 0});
  RoundTrip("active marker survives",
            ObjectiveMarkerState{.active = true, .quest = 0x000a1234ull,
                                 .x = 1.0f, .y = 2.0f, .z = 3.0f});
  RoundTrip("negative positions survive",
            ObjectiveMarkerState{.active = true, .quest = 0xffffffffffffffffull,
                                 .x = -1234.5f, .y = -0.0f, .z = -987654.0f});
  RoundTrip("fractional positions survive",
            ObjectiveMarkerState{.active = true, .quest = 0x42ull,
                                 .x = 0.125f, .y = -0.333333f, .z = 65536.5f});
}

void TestSizeRejection() {
  std::puts("size rejection:");

  // Empty buffer has nothing to read.
  Check("empty buffer rejected", !DecodeObjectiveMarker(ByteSpan()).has_value());

  std::vector<rx::u8> valid = EncodeObjectiveMarker(
      ObjectiveMarkerState{.active = true, .quest = 7, .x = 1, .y = 2, .z = 3});

  // Every truncation must be rejected and must never read out of bounds.
  bool every_truncation_rejected = true;
  for (size_t cut = 0; cut < valid.size(); ++cut) {
    std::vector<rx::u8> shorter(valid.begin(), valid.begin() + cut);
    if (DecodeObjectiveMarker(shorter).has_value()) {
      every_truncation_rejected = false;
    }
  }
  Check("every truncation rejected", every_truncation_rejected);

  // One trailing byte makes the buffer oversized and is rejected.
  std::vector<rx::u8> oversized = valid;
  oversized.push_back(0);
  Check("oversized buffer rejected",
        !DecodeObjectiveMarker(oversized).has_value());
}

}  // namespace

int main() {
  TestRoundTrip();
  TestSizeRejection();
  if (g_failures == 0) {
    std::puts("objective_marker_net: all checks passed");
    return 0;
  }
  std::printf("objective_marker_net: %d checks FAILED\n", g_failures);
  return 1;
}
