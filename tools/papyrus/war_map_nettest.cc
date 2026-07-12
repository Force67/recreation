// war_map_nettest: deterministic round-trip checks for the Civil War board
// codec replicated server -> client. No game data needed, so it runs in ctest.

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "core/types.h"
#include "gamenet/war_map_net.h"

namespace {

using rx::ByteSpan;
using rx::net::DecodeWarMap;
using rx::net::EncodeWarMap;
using rx::net::WarMapHold;
using rx::net::WarMapState;

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

bool Same(const WarMapState& a, const WarMapState& b) {
  if (a.imperial_fraction != b.imperial_fraction) return false;
  if (a.holds.size() != b.holds.size()) return false;
  for (size_t i = 0; i < a.holds.size(); ++i)
    if (a.holds[i].name != b.holds[i].name || a.holds[i].owner != b.holds[i].owner) return false;
  return true;
}

void RoundTrip(const char* what, const WarMapState& m) {
  std::vector<rx::u8> blob = EncodeWarMap(m);
  std::optional<WarMapState> decoded = DecodeWarMap(blob);
  Check(what, decoded.has_value() && Same(m, *decoded));
}

void TestRoundTrip() {
  std::puts("war map round trip:");

  RoundTrip("empty board survives", WarMapState{});

  WarMapState skyrim;
  skyrim.imperial_fraction = 0.5f;
  skyrim.holds = {{"Solitude", 1}, {"Markarth", 1},  {"Falkreath", 1},
                  {"Morthal", 1},  {"Whiterun", 0},  {"Windhelm", 2},
                  {"Riften", 2},   {"Winterhold", 2}, {"Dawnstar", 2}};
  RoundTrip("the nine holds survive", skyrim);

  WarMapState after_capture = skyrim;
  after_capture.holds[4].owner = 1;  // Whiterun falls to the Legion
  after_capture.imperial_fraction = 5.0f / 9.0f;
  RoundTrip("a captured hold survives", after_capture);

  WarMapState empty_name;
  empty_name.holds = {{"", 0}, {"X", 2}};
  RoundTrip("empty and one-char names survive", empty_name);
}

void TestTruncation() {
  std::puts("truncation rejection:");

  Check("empty buffer rejected", !DecodeWarMap(ByteSpan()).has_value());

  WarMapState m;
  m.imperial_fraction = 0.25f;
  m.holds = {{"Whiterun", 1}, {"Riften", 2}};
  std::vector<rx::u8> valid = EncodeWarMap(m);

  // Every truncation must be rejected and must never read out of bounds.
  bool every_truncation_rejected = true;
  for (size_t cut = 0; cut < valid.size(); ++cut) {
    std::vector<rx::u8> shorter(valid.begin(), valid.begin() + cut);
    if (DecodeWarMap(shorter).has_value()) every_truncation_rejected = false;
  }
  Check("every truncation rejected", every_truncation_rejected);
}

}  // namespace

int main() {
  TestRoundTrip();
  TestTruncation();
  if (g_failures == 0) {
    std::puts("war_map_net: all checks passed");
    return 0;
  }
  std::printf("war_map_net: %d checks FAILED\n", g_failures);
  return 1;
}
