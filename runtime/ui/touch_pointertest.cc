// touch_pointertest: the HUD's touch-to-pointer state machine
// (runtime/ui/touch_pointer.h). This is the logic a Steam Deck would otherwise
// be the only way to exercise, and its failure modes are all silent-but-wrong
// rather than crashes: a tap that lands on the wrong widget because the pointer
// had not moved yet, a list entry selected on the way into a two-finger scroll,
// a button left stuck down when a gesture ends, or touch and mouse both driving
// the pointer in the same pump. Pure logic, no window or gpu, so it runs in the
// ctest gate.

#include <cmath>
#include <cstdio>

#include "runtime/ui/touch_pointer.h"

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    ++g_failures;
}

bool Near(rx::f32 a, rx::f32 b) {
  return std::fabs(a - b) < 1e-4f;
}

using rx::ResolveTouchPointer;
using rx::TouchPointerEvents;
using rx::TouchPointerState;
using rx::TouchState;
using Phase = rx::TouchState::Phase;

// With nothing on the panel the caller must fall back to the mouse, and only
// then; touch and mouse must never both drive the pointer in one pump.
void TestIdleDefersToMouse() {
  std::printf("idle defers to the mouse\n");
  TouchState t;
  TouchPointerState s;

  t.BeginPump();
  const TouchPointerEvents e = ResolveTouchPointer(t, s, 1.0f, 1.0f);
  Check("mouse feed requested", e.use_mouse);
  Check("no pointer move from touch", !e.move);
  Check("no button from touch", !e.button);
}

// A tap must move the pointer before pressing, or the gui hit-tests the click
// against wherever the pointer happened to be last.
void TestTapMovesBeforePressing() {
  std::printf("tap moves before pressing\n");
  TouchState t;
  TouchPointerState s;

  t.BeginPump();
  t.Apply(1, 300.0f, 400.0f, 1.0f, Phase::kDown);
  TouchPointerEvents e = ResolveTouchPointer(t, s, 1.0f, 1.0f);
  Check("pointer moved to the contact", e.move && Near(e.x, 300.0f) && Near(e.y, 400.0f));
  Check("button went down", e.button && e.down);
  Check("mouse feed suppressed while touching", !e.use_mouse);

  // Holding still: no repeated button edge.
  t.BeginPump();
  e = ResolveTouchPointer(t, s, 1.0f, 1.0f);
  Check("held finger emits no new button edge", !e.button);
  Check("held finger keeps updating the pointer", e.move);

  // Lift.
  t.Apply(1, 300.0f, 400.0f, 0.0f, Phase::kUp);
  e = ResolveTouchPointer(t, s, 1.0f, 1.0f);
  Check("lift releases the button", e.button && !e.down);

  t.BeginPump();
  e = ResolveTouchPointer(t, s, 1.0f, 1.0f);
  Check("after the lift the mouse feed resumes", e.use_mouse);
  Check("no stray button after the lift", !e.button);
}

// The canvas can be larger than the window; the contact has to be scaled the
// same way the mouse is or taps land off-target.
void TestScaleAppliesToContact() {
  std::printf("canvas scale applies to the contact\n");
  TouchState t;
  TouchPointerState s;

  t.BeginPump();
  t.Apply(1, 100.0f, 50.0f, 1.0f, Phase::kDown);
  const TouchPointerEvents e = ResolveTouchPointer(t, s, 2.0f, 3.0f);
  Check("x scaled into canvas space", Near(e.x, 200.0f));
  Check("y scaled into canvas space", Near(e.y, 150.0f));
}

// Two fingers scroll, and the click the first one started must be taken back.
void TestTwoFingersScrollAndCancelTheClick() {
  std::printf("two fingers scroll and cancel the click\n");
  TouchState t;
  TouchPointerState s;

  t.BeginPump();
  t.Apply(1, 100.0f, 100.0f, 1.0f, Phase::kDown);
  TouchPointerEvents e = ResolveTouchPointer(t, s, 1.0f, 1.0f);
  Check("first finger pressed", e.button && e.down);

  // Second finger lands: this is a scroll, not a click.
  t.BeginPump();
  t.Apply(2, 140.0f, 100.0f, 1.0f, Phase::kDown);
  e = ResolveTouchPointer(t, s, 1.0f, 1.0f);
  Check("the pending click is cancelled", e.button && !e.down);
  Check("no pointer move during a scroll", !e.move);

  // Drag both down by 80px -> two notches at 40px each.
  t.BeginPump();
  t.Apply(1, 100.0f, 180.0f, 1.0f, Phase::kMove);
  t.Apply(2, 140.0f, 180.0f, 1.0f, Phase::kMove);
  e = ResolveTouchPointer(t, s, 1.0f, 1.0f);
  Check("scroll emitted", e.scroll);
  Check("scroll is the averaged travel in notches", Near(e.scroll_y, 2.0f));
  Check("scrolling never clicks", !e.button);

  // The 1:1 property. ugui turns a notch into this many pixels of scroll offset
  // (input.cc), so emitting travel/40 notches must move the content by exactly
  // the distance the fingers travelled. If these two constants ever drift the
  // content slides faster or slower than the finger, which reads as broken.
  constexpr rx::f32 kUguiPixelsPerNotch = 40.0f;
  {
    TouchState u;
    TouchPointerState us;
    u.BeginPump();
    u.Apply(1, 0.0f, 0.0f, 1.0f, Phase::kDown);
    u.Apply(2, 40.0f, 0.0f, 1.0f, Phase::kDown);
    ResolveTouchPointer(u, us, 1.0f, 1.0f);
    u.BeginPump();
    const rx::f32 travel = 123.0f;
    u.Apply(1, 0.0f, travel, 1.0f, Phase::kMove);
    u.Apply(2, 40.0f, travel, 1.0f, Phase::kMove);
    const TouchPointerEvents ue = ResolveTouchPointer(u, us, 1.0f, 1.0f);
    Check("content travel equals finger travel (1:1)",
          ue.scroll && Near(ue.scroll_y * kUguiPixelsPerNotch, travel));
  }

  // Opposed movement averages out rather than scrolling wildly (a pinch).
  t.BeginPump();
  t.Apply(1, 100.0f, 220.0f, 1.0f, Phase::kMove);
  t.Apply(2, 140.0f, 140.0f, 1.0f, Phase::kMove);
  e = ResolveTouchPointer(t, s, 1.0f, 1.0f);
  Check("a pinch does not scroll", !e.scroll || Near(e.scroll_y, 0.0f));
}

// The case that made the latch necessary: lifting one finger out of a scroll
// leaves a contact behind, which must not be read as a fresh tap.
void TestGestureTailNeverTaps() {
  std::printf("gesture tail never taps\n");
  TouchState t;
  TouchPointerState s;

  t.BeginPump();
  t.Apply(1, 100.0f, 100.0f, 1.0f, Phase::kDown);
  t.Apply(2, 140.0f, 100.0f, 1.0f, Phase::kDown);
  ResolveTouchPointer(t, s, 1.0f, 1.0f);
  Check("gesture latched", s.gesture);

  // One finger comes off; one is still down.
  t.BeginPump();
  t.Apply(2, 140.0f, 100.0f, 0.0f, Phase::kUp);
  TouchPointerEvents e = ResolveTouchPointer(t, s, 1.0f, 1.0f);
  Check("remaining finger does not press", !(e.button && e.down));
  Check("remaining finger does not move the pointer", !e.move);
  Check("still not falling through to the mouse", !e.use_mouse);

  // The last finger comes off too.
  t.BeginPump();
  t.Apply(1, 100.0f, 100.0f, 0.0f, Phase::kUp);
  e = ResolveTouchPointer(t, s, 1.0f, 1.0f);
  Check("last lift still does not click", !(e.button && e.down));

  t.BeginPump();
  e = ResolveTouchPointer(t, s, 1.0f, 1.0f);
  Check("latch clears once the panel is empty", !s.gesture);
  Check("mouse feed resumes", e.use_mouse);

  // And a genuine tap after the gesture works again.
  t.Apply(3, 50.0f, 60.0f, 1.0f, Phase::kDown);
  e = ResolveTouchPointer(t, s, 1.0f, 1.0f);
  Check("a fresh tap after the gesture presses", e.button && e.down);
}

// A contact that vanishes without a lift (focus loss) must not strand the
// button down, or the gui thinks the mouse is held forever.
void TestVanishedContactReleases() {
  std::printf("vanished contact releases\n");
  TouchState t;
  TouchPointerState s;

  t.BeginPump();
  t.Apply(1, 10.0f, 10.0f, 1.0f, Phase::kDown);
  TouchPointerEvents e = ResolveTouchPointer(t, s, 1.0f, 1.0f);
  Check("pressed", e.button && e.down);

  // The whole state is dropped, as a focus loss would.
  TouchState empty;
  e = ResolveTouchPointer(empty, s, 1.0f, 1.0f);
  Check("button is released, not left stuck", e.button && !e.down);
  Check("mouse feed resumes", e.use_mouse);
  Check("latch is clear", !s.prev_down);
}

}  // namespace

int main() {
  std::printf("hud touch pointer\n");
  TestIdleDefersToMouse();
  TestTapMovesBeforePressing();
  TestScaleAppliesToContact();
  TestTwoFingersScrollAndCancelTheClick();
  TestGestureTailNeverTaps();
  TestVanishedContactReleases();

  if (g_failures != 0) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
