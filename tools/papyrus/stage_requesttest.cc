// stage_requesttest: round-trips the client -> server quest-debugger channel.
// No game data needed (requests are built by hand), so it runs in ctest.

#include <cstdio>
#include <optional>
#include <vector>

#include "core/types.h"
#include "gamenet/stage_request.h"

// zetanet headers (pulled in transitively elsewhere) inject their own scalar
// aliases, so the scalar types stay fully qualified as rx:: throughout.
namespace {

using rx::ByteSpan;
using rx::net::DecodeStageRequest;
using rx::net::EncodeStageRequest;
using rx::net::StageOp;
using rx::net::StageRequest;

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

bool Same(const StageRequest& a, const StageRequest& b) {
  return a.quest == b.quest && a.op == b.op && a.a == b.a && a.b == b.b;
}

void TestRoundTrip(const char* what, const StageRequest& req) {
  std::vector<rx::u8> blob = EncodeStageRequest(req);
  Check("fixed wire size", blob.size() == 17);
  std::optional<StageRequest> back = DecodeStageRequest(blob);
  Check(what, back.has_value() && Same(req, *back));
}

}  // namespace

int main() {
  std::puts("stage request round trip:");

  TestRoundTrip("set stage (negative survives)",
                {0x000a1234ull, StageOp::kSetStage, /*a=*/-1, /*b=*/0});
  TestRoundTrip("set running on",
                {0x000b5678ull, StageOp::kSetRunning, /*a=*/0, /*b=*/1});
  TestRoundTrip("objective displayed off",
                {0xffffffffull, StageOp::kSetObjectiveDisplayed, /*a=*/3, /*b=*/0});
  TestRoundTrip("objective completed on",
                {0xdeadbeefcafef00dull, StageOp::kSetObjectiveCompleted,
                 /*a=*/7, /*b=*/1});

  std::puts("rejection:");

  // Wrong size in either direction is rejected, never read out of bounds.
  std::vector<rx::u8> good = EncodeStageRequest(
      {0x10ull, StageOp::kSetStage, /*a=*/5, /*b=*/0});
  Check("empty buffer rejected", !DecodeStageRequest(ByteSpan()).has_value());
  for (size_t cut = 0; cut < good.size(); ++cut) {
    std::vector<rx::u8> shorter(good.begin(), good.begin() + cut);
    if (DecodeStageRequest(shorter).has_value()) {
      Check("truncated buffer rejected", false);
    }
  }
  std::vector<rx::u8> longer = good;
  longer.push_back(0x00);
  Check("oversized buffer rejected", !DecodeStageRequest(longer).has_value());

  // An out-of-range op byte (only 0..3 are valid) must be rejected.
  std::vector<rx::u8> bad_op = good;
  bad_op[8] = 0xff;
  Check("unknown op rejected", !DecodeStageRequest(bad_op).has_value());

  if (g_failures == 0) {
    std::puts("stage_request: all checks passed");
    return 0;
  }
  std::printf("stage_request: %d checks FAILED\n", g_failures);
  return 1;
}
