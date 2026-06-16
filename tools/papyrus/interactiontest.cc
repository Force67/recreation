// interactiontest: checks the activation-target picker (the "press E to use
// what you are looking at" selection). Pure geometry, no game data.

#include <cstdio>

#include "world/interaction.h"

using rec::world::ActivationCandidate;
using rec::world::PickActivationTarget;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

}  // namespace

int main() {
  const float origin[3] = {0, 0, 0};
  const float forward[3] = {0, 0, 1};  // looking down +z

  std::puts("activation picker:");

  // Nothing in range.
  {
    ActivationCandidate c[1] = {{1, {0, 0, 100}}};
    Check("out of range -> none", PickActivationTarget(origin, forward, c, 1, 50.0f, 0.5f) == -1);
  }
  // Dead ahead, in range.
  {
    ActivationCandidate c[1] = {{7, {0, 0, 10}}};
    Check("ahead and near -> picked", PickActivationTarget(origin, forward, c, 1, 50.0f, 0.5f) == 0);
  }
  // Behind the player is never picked.
  {
    ActivationCandidate c[1] = {{7, {0, 0, -10}}};
    Check("behind -> none", PickActivationTarget(origin, forward, c, 1, 50.0f, 0.5f) == -1);
  }
  // Off to the side beyond the cone is rejected.
  {
    ActivationCandidate c[1] = {{7, {30, 0, 1}}};  // mostly sideways
    Check("outside cone -> none", PickActivationTarget(origin, forward, c, 1, 50.0f, 0.5f) == -1);
  }
  // The most centered of several candidates wins, not the nearest.
  {
    ActivationCandidate c[2] = {
        {1, {5, 0, 5}},   // near but off-axis
        {2, {0, 0, 12}},  // farther but dead ahead
    };
    int pick = PickActivationTarget(origin, forward, c, 2, 50.0f, 0.5f);
    Check("most centered wins over nearest", pick == 1);
  }
  // Height difference pushes a target out of the cone.
  {
    ActivationCandidate c[1] = {{7, {0, 40, 5}}};  // high above, ahead in z
    Check("too high -> none", PickActivationTarget(origin, forward, c, 1, 50.0f, 0.7f) == -1);
  }

  if (g_failures == 0) {
    std::puts("interaction: all checks passed");
    return 0;
  }
  std::printf("interaction: %d checks FAILED\n", g_failures);
  return 1;
}
