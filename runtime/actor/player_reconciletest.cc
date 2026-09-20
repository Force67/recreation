// player_reconciletest: the rule that pulls a client's own body back onto the
// one the host simulates. The whole point is reluctance -- correcting jitter is
// rubber-banding, and correcting the vertical axis fights a jump -- so this
// covers where each band begins, that easing closes part of a gap rather than
// all of it, that a jump is not treated as an error, and that falling through
// the world still is.

#include <cmath>
#include <cstdio>

#include "runtime/actor/player_reconcile.h"

namespace pr = rx::player_reconcile;
using rx::Vec3;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    ++g_failures;
}

constexpr float kDt = 1.0f / 60.0f;

float Dist2D(const Vec3& a, const Vec3& b) {
  const float dx = a.x - b.x, dz = a.z - b.z;
  return std::sqrt(dx * dx + dz * dz);
}

}  // namespace

int main() {
  std::printf("player_reconciletest\n");

  const pr::Settings s;  // the shipped thresholds
  const Vec3 here{10.0f, 4.0f, -20.0f};

  // Agreement, and the jitter band just inside it.
  Check("no gap is accepted", pr::Reconcile(here, here, kDt, s).action == pr::Action::kAccept);
  Check("jitter is left alone",
        pr::Reconcile(here, Vec3{here.x + 0.2f, here.y, here.z}, kDt, s).action ==
            pr::Action::kAccept);

  // The easing band.
  const Vec3 off{here.x + 1.0f, here.y, here.z};
  const pr::Correction eased = pr::Reconcile(here, off, kDt, s);
  Check("a walkable gap eases", eased.action == pr::Action::kEase);
  Check("easing closes part of the gap, not all of it",
        Dist2D(eased.position, off) > 0.0f && Dist2D(eased.position, off) < 1.0f);
  Check("easing moves toward the host, not away",
        Dist2D(eased.position, off) < Dist2D(here, off));
  Check("a whole second closes the whole gap",
        Dist2D(pr::Reconcile(here, off, 1.0f, s).position, off) < 0.001f);
  Check("a zero-length frame moves nothing",
        Dist2D(pr::Reconcile(here, off, 0.0f, s).position, here) < 0.001f);

  // The snap band.
  const Vec3 far_off{here.x + 5.0f, here.y, here.z};
  const pr::Correction snapped = pr::Reconcile(here, far_off, kDt, s);
  Check("a gap too large to walk off snaps", snapped.action == pr::Action::kSnap);
  Check("a snap lands exactly where the host says",
        snapped.position.x == far_off.x && snapped.position.y == far_off.y &&
            snapped.position.z == far_off.z);

  // Height: a jump is timing, not error.
  const pr::Correction jumping = pr::Reconcile(Vec3{here.x, here.y + 1.5f, here.z}, here, kDt, s);
  Check("a jump the host has not applied yet is not an error",
        jumping.action == pr::Action::kAccept);
  // ...and while easing a horizontal gap, the local height is kept.
  const pr::Correction mid_air =
      pr::Reconcile(Vec3{here.x, here.y + 1.5f, here.z}, off, kDt, s);
  Check("easing keeps the local height", mid_air.action == pr::Action::kEase &&
                                             mid_air.position.y == here.y + 1.5f);

  // ...but a hole in the world is.
  Check("a vertical gap too large to be a jump snaps",
        pr::Reconcile(Vec3{here.x, here.y - 40.0f, here.z}, here, kDt, s).action ==
            pr::Action::kSnap);

  // The thresholds are the caller's; a server that wants no easing can say so.
  pr::Settings eager;
  eager.ignore_m = 0.0f;
  Check("a zero ignore band corrects everything",
        pr::Reconcile(here, Vec3{here.x + 0.01f, here.y, here.z}, kDt, eager).action ==
            pr::Action::kEase);

  std::printf("player_reconciletest: %d failure(s)\n", g_failures);
  return g_failures ? 1 : 0;
}
