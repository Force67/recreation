#ifndef RECREATION_UI_TOUCH_POINTER_H_
#define RECREATION_UI_TOUCH_POINTER_H_

#include "core/input.h"

namespace rx {

// Turns a pump's touch contacts into the pointer events the HUD's immediate
// mode gui understands. ugui is a single-pointer UI, so this is where the
// handheld's multi-touch panel is reduced to "move the pointer", "click", or
// "scroll", and where the awkward cases live:
//
//   - One finger drives the pointer. The move must be emitted before the press:
//     a mouse establishes its position over frames of hover, a finger arrives
//     already on the widget, so the gui has to be told where the pointer is
//     before it hit-tests the click.
//   - Two fingers scroll, and the click the first finger started is cancelled,
//     or a list selects an entry on the way into the gesture.
//   - Once a multi-finger gesture starts, no finger may click until every one
//     of them is off. Without that latch, lifting the second finger out of a
//     scroll leaves one contact behind and reads as a tap.
//   - With no contacts at all the caller should run its normal mouse feed;
//     touch and mouse must never both drive the pointer in one pump.
//
// Kept out of the gui translation unit so the state machine can be tested
// without a window, a gpu or a panel (see touch_pointertest).

// Carried across pumps by the caller.
struct TouchPointerState {
  bool prev_down = false;  // pointer button state this produced last pump
  bool gesture = false;    // a multi-finger gesture is in progress
};

// What to feed the gui this pump. Emit in field order: move, then button.
struct TouchPointerEvents {
  bool move = false;
  f32 x = 0;
  f32 y = 0;

  bool button = false;  // the pointer button changed state
  bool down = false;

  bool scroll = false;
  f32 scroll_y = 0;  // wheel notches

  // No touch input this pump; the caller runs its mouse feed instead.
  bool use_mouse = false;
};

// Scale converts window pixels to the gui's canvas (they differ on HiDPI and
// when the swapchain is clamped).
//
// This is NOT a free tuning value: it is coupled to ugui's own wheel constant.
// ugui turns one notch into 40px of scroll offset and negates it
// (input.cc: ScrollWidget(world, w, {-delta.x * 40, -delta.y * 40})), and the
// paint pass draws children at rect - offset. So converting finger travel at
// this same 40 gives, for a drag of D canvas pixels:
//
//   scroll_y = D / 40  ->  offset += -(D / 40) * 40 = -D  ->  content moves +D
//
// which is content tracking the finger exactly 1:1, the direct-manipulation
// behaviour a touchscreen wants. Changing this without changing ugui's constant
// makes the content slide faster or slower than the finger.
inline constexpr f32 kTouchPixelsPerNotch = 40.0f;

inline TouchPointerEvents ResolveTouchPointer(const TouchState& touch, TouchPointerState& state,
                                              f32 scale_x, f32 scale_y) {
  TouchPointerEvents out;

  // Released slots linger one pump so a tap end is never missed; they are not
  // contacts any more, so the gesture choice ignores them.
  const TouchPoint* finger = nullptr;
  u32 contacts = 0;
  for (u32 i = 0; i < touch.count; ++i) {
    if (touch.points[i].released) continue;
    if (!finger) finger = &touch.points[i];
    ++contacts;
  }
  // A lone released point still has to deliver its lift.
  if (!finger && touch.count > 0) finger = &touch.points[0];

  if (touch.count == 0) state.gesture = false;  // panel clear, gesture over
  if (contacts >= 2) state.gesture = true;

  auto release = [&] {
    if (!state.prev_down) return;
    out.button = true;
    out.down = false;
    state.prev_down = false;
  };

  if (contacts >= 2) {
    release();
    f32 dy = 0;
    u32 n = 0;
    for (u32 i = 0; i < touch.count && n < 2; ++i) {
      if (touch.points[i].released) continue;
      dy += touch.points[i].dy;
      ++n;
    }
    if (n > 0 && dy != 0.0f) {
      out.scroll = true;
      out.scroll_y = (dy / static_cast<f32>(n)) * scale_y / kTouchPixelsPerNotch;
    }
    return out;
  }

  if (state.gesture) {
    // Tail of a gesture: fingers are still coming off and none may click.
    release();
    return out;
  }

  if (finger) {
    out.move = true;
    out.x = finger->x * scale_x;
    out.y = finger->y * scale_y;
    const bool down = !finger->released;
    if (down != state.prev_down) {
      out.button = true;
      out.down = down;
      state.prev_down = down;
    }
    return out;
  }

  release();  // contact vanished without a lift; never leave the button stuck
  out.use_mouse = true;
  return out;
}

}  // namespace rx

#endif  // RECREATION_UI_TOUCH_POINTER_H_
