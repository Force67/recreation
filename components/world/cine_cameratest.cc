// cine_cameratest: the dialogue camera's geometry and cutting policy. Checks each
// shot puts the lens where it should be relative to the two heads, that the
// director cuts on a new speaker but not faster than its minimum hold, and that a
// long speech eventually gets a new angle. Pure, no game data.

#include <cmath>
#include <cstdio>

#include "components/world/cine_camera.h"

using namespace rx;
// rx::u64/i64 (long) and base/arch.h's (long long) are different types sharing
// a global name, so the 64-bit spellings below are qualified; the other scalars
// agree between the two and need no help.
using namespace rx::world;

namespace {

int g_failures = 0;
void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

f32 Dist(const f32 a[3], const f32 b[3]) {
  const f32 dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void TestShots() {
  std::puts("cine camera (shot geometry):");
  // Speaker at the origin, listener 3 m down +X, both at head height.
  const f32 speaker[3] = {0, 1.7f, 0};
  const f32 listener[3] = {3, 1.7f, 0};
  const ShotParams p;

  const CineFraming over = SolveShot(ShotKind::kOverShoulder, speaker, listener, 0, p);
  Check("an over-shoulder looks at the speaker", Dist(over.target, speaker) < 0.01f);
  Check("it sits on the listener's side of the axis", over.eye[0] > listener[0]);
  Check("it is offset off the axis so the near head does not block the lens",
        std::fabs(over.eye[2]) > 0.3f);
  Check("it rides just above the eyeline", over.eye[1] > speaker[1]);

  const CineFraming reverse = SolveShot(ShotKind::kReverse, speaker, listener, 0, p);
  Check("the reverse looks at the listener", Dist(reverse.target, listener) < 0.01f);
  Check("and shoots from the far side of the speaker", reverse.eye[0] < speaker[0]);
  Check("the two angles sit on opposite sides of the axis",
        (over.eye[2] > 0) != (reverse.eye[2] > 0));

  const CineFraming two = SolveShot(ShotKind::kTwoShot, speaker, listener, 0, p);
  Check("a two-shot looks between them",
        std::fabs(two.target[0] - 1.5f) < 0.01f && std::fabs(two.target[2]) < 0.01f);
  Check("from off to one side", std::fabs(two.eye[2]) > 1.0f);

  const CineFraming close = SolveShot(ShotKind::kCloseUp, speaker, listener, 0, p);
  Check("a close-up is tight on the speaker",
        Dist(close.eye, speaker) < p.close + 0.2f && Dist(close.target, speaker) < 0.01f);

  const CineFraming wide = SolveShot(ShotKind::kWide, speaker, listener, 0, p);
  Check("a wide pulls back and up",
        Dist(wide.eye, wide.target) > Dist(two.eye, two.target) && wide.eye[1] > two.eye[1]);

  // A lone speaker (no distinct listener) still gets a framed angle from the yaw.
  const CineFraming solo = SolveShot(ShotKind::kOverShoulder, speaker, speaker, 0, p);
  Check("a lone speaker is framed off their facing, not from inside their head",
        Dist(solo.eye, speaker) > 0.5f);
}

void TestDirector() {
  std::puts("cine camera (cutting):");
  ShotDirector d;
  d.set_min_shot(1.0f);
  d.set_max_shot(5.0f);
  Check("the first frame cuts in", d.Update(0.016f, 1, 2));
  const ShotKind first = d.kind();
  Check("holding on the same speaker does not cut", !d.Update(0.5f, 1, 2));
  Check("a new speaker inside the minimum hold waits", !d.Update(0.2f, 2, 1));
  Check("and cuts once the hold has elapsed", d.Update(0.5f, 2, 1));
  Check("the angle changed with the speaker", d.kind() != first || d.cuts() == 1);

  // A single long speech: no speaker change, but the camera must find a new size
  // rather than sitting on one angle for the whole monologue.
  ShotDirector mono;
  mono.set_min_shot(1.0f);
  mono.set_max_shot(3.0f);
  mono.Update(0.016f, 7, 8);
  bool cut = false;
  for (int i = 0; i < 400 && !cut; ++i) cut = mono.Update(0.016f, 7, 8);
  Check("a long hold cuts to another angle", cut);

  mono.Reset();
  Check("reset re-cuts on the next frame", mono.Update(0.016f, 7, 8) && mono.cuts() == 0);
}

void TestEase() {
  std::puts("cine camera (easing):");
  CineFraming from;
  CineFraming to;
  to.eye[0] = 10.0f;
  to.target[1] = 4.0f;
  const CineFraming mid = EaseFraming(from, to, 0.1f, 6.0f);
  Check("easing moves toward the goal without arriving", mid.eye[0] > 0.0f && mid.eye[0] < 10.0f);
  CineFraming cur = from;
  for (int i = 0; i < 200; ++i) cur = EaseFraming(cur, to, 0.016f, 6.0f);
  Check("and converges on it",
        std::fabs(cur.eye[0] - 10.0f) < 0.05f && std::fabs(cur.target[1] - 4.0f) < 0.05f);
}

void TestPushIn() {
  std::puts("cine camera (push-in while a shot is held):");
  CineFraming shot;
  shot.eye[0] = 0; shot.eye[1] = 2; shot.eye[2] = 4;
  shot.target[0] = 0; shot.target[1] = 2; shot.target[2] = 0;
  const CineFraming at_cut = PushIn(shot, 0.0f);
  Check("a fresh shot is the solved framing", std::fabs(at_cut.eye[2] - 4.0f) < 1e-4f);
  const CineFraming held = PushIn(shot, 4.0f);
  Check("a held shot creeps toward the subject", held.eye[2] < 3.99f && held.eye[2] > 3.7f);
  const CineFraming forever = PushIn(shot, 1000.0f);
  Check("the creep is capped well short of the subject", forever.eye[2] > 3.4f);
  Check("the aim point never moves", std::fabs(forever.target[2]) < 1e-4f);
}

}  // namespace

int main() {
  TestPushIn();
  TestShots();
  TestDirector();
  TestEase();
  if (g_failures) {
    std::printf("cine camera: %d check(s) FAILED\n", g_failures);
    return 1;
  }
  std::puts("cine camera: all checks passed");
  return 0;
}
