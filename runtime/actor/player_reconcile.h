#ifndef RECREATION_RUNTIME_ACTOR_PLAYER_RECONCILE_H_
#define RECREATION_RUNTIME_ACTOR_PLAYER_RECONCILE_H_

#include <base/algorithm.h>

#include <cmath>

#include "core/math.h"
#include "core/types.h"

namespace rx::player_reconcile {

// Movement is server-simulated: a client streams its character intent, the host
// runs the same character pipeline, and every client receives the host's
// transforms. A client also simulates its OWN body locally, so input feels
// instant rather than a round trip late -- which leaves two copies of one body
// free to drift apart. The host applies your intent half a round trip late, a
// lost packet skips an intent entirely, and a collision resolved a frame apart
// puts you on different sides of a rock. Nothing pulls the two back together, so
// the gap only grows, and the body other players see is the host's: you end up
// standing somewhere you are not.
//
// This is the correction, and it is deliberately reluctant. Chasing a small
// disagreement is what rubber-banding IS, so a small one is left alone; a
// moderate one is closed smoothly over a few frames; only a gap too large to
// walk off is snapped.
//
// The horizontal and vertical axes are judged apart. Jumping and falling put a
// metre between the copies through nothing but timing, and easing against that
// fights the jump itself, so the vertical axis is never eased -- only snapped,
// at a gap that means "fell through the world" rather than "jumped".

struct Settings {
  f32 ignore_m = 0.25f;        // below this, a horizontal gap is jitter
  f32 snap_m = 3.0f;           // at or above this, teleport instead of easing
  f32 snap_vertical_m = 6.0f;  // a vertical gap this large is not a jump
  f32 ease_per_second = 5.0f;  // fraction of the horizontal gap closed a second
};

enum class Action {
  kAccept,  // the two copies agree closely enough: leave the body alone
  kEase,    // close part of the gap this frame
  kSnap,    // put the body where the host says it is
};

struct Correction {
  Action action = Action::kAccept;
  Vec3 position{};  // where to put the local body (kEase and kSnap)
};

// `local_feet` is where this client has its own body, `server_feet` where the
// host's snapshot says it is; both are engine-space feet positions (a character's
// scene::Transform is its feet, on the host and on the client alike).
inline Correction Reconcile(const Vec3& local_feet,
                            const Vec3& server_feet,
                            f32 dt,
                            const Settings& settings = {}) {
  const f32 dx = server_feet.x - local_feet.x;
  const f32 dy = server_feet.y - local_feet.y;
  const f32 dz = server_feet.z - local_feet.z;
  const f32 horizontal = std::sqrt(dx * dx + dz * dz);

  if (horizontal >= settings.snap_m || std::fabs(dy) >= settings.snap_vertical_m)
    return {Action::kSnap, server_feet};
  if (horizontal < settings.ignore_m)
    return {Action::kAccept, local_feet};
  // Horizontally only: the vertical axis belongs to whatever the local body is
  // doing, which the host is a moment behind on.
  const f32 closed = base::Min(1.0f, settings.ease_per_second * (dt > 0.0f ? dt : 0.0f));
  return {Action::kEase,
          Vec3{local_feet.x + dx * closed, local_feet.y, local_feet.z + dz * closed}};
}

}  // namespace rx::player_reconcile

#endif  // RECREATION_RUNTIME_ACTOR_PLAYER_RECONCILE_H_
