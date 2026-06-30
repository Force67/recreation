#ifndef RECREATION_WORLD_COMBAT_H_
#define RECREATION_WORLD_COMBAT_H_

#include <cmath>
#include <mutex>
#include <vector>

#include "core/types.h"

// Pure melee-combat math plus the thread-safe channel that carries combat
// enrollment from the guest thread (the Skyrim bindings' StartCombat/StopCombat
// and death notification) to the main-thread combat driver. Like npc_ai.h and
// objective_marker.h this header is dependency-light so the geometry can be unit
// tested without the ECS or the renderer.
namespace rec::world {

// Tunables for the melee combat simulation, in engine space (metres, seconds).
struct CombatParams {
  f32 melee_reach = 2.6f;       // a swing connects within this planar distance
  f32 engage_radius = 40.0f;    // auto-acquire a hostile within this planar range
  f32 give_up_radius = 70.0f;   // drop a target that flees beyond this
  f32 swing_seconds = 1.3f;     // seconds between swings
  f32 approach_speed = 3.6f;    // run speed while closing on a target
  f32 base_damage = 22.0f;      // health removed per connected swing
  f32 damage_variance = 0.35f;  // +/- fraction applied to base_damage per swing
};

// Squared planar (XZ) distance between two engine-space points (Y ignored: melee
// reach and target acquisition are ground-plane concepts, like the steering).
inline f32 PlanarDist2(const f32 a[3], const f32 b[3]) {
  const f32 dx = a[0] - b[0], dz = a[2] - b[2];
  return dx * dx + dz * dz;
}

inline bool WithinPlanar(const f32 a[3], const f32 b[3], f32 radius) {
  return PlanarDist2(a, b) <= radius * radius;
}

// Index of the nearest point in pts (n entries, stride 3 floats) to self within
// radius, or -1 if none. The caller passes only living hostiles, so the result
// is the target an idle combatant should engage.
inline int NearestWithin(const f32 self[3], const f32* pts, int n, f32 radius) {
  int best = -1;
  f32 best_d2 = radius * radius;
  for (int i = 0; i < n; ++i) {
    const f32 d2 = PlanarDist2(self, pts + i * 3);
    if (d2 < best_d2) {
      best_d2 = d2;
      best = i;
    }
  }
  return best;
}

// True when target is within `reach` of self AND inside the forward swing arc:
// the cosine of the angle between the facing dir (fwd, a planar unit vector) and
// the direction to the target is at least `arc_cos`. Drives the player's melee
// target pick (it strikes what it faces, not what is behind it).
inline bool InMeleeArc(const f32 self[3], const f32 target[3], const f32 fwd[3], f32 reach,
                       f32 arc_cos) {
  const f32 dx = target[0] - self[0], dz = target[2] - self[2];
  const f32 d2 = dx * dx + dz * dz;
  if (d2 > reach * reach || d2 < 1e-4f) return false;
  const f32 inv = 1.0f / std::sqrt(d2);
  return (dx * fwd[0] + dz * fwd[2]) * inv >= arc_cos;
}

// Damage for one connected swing: base scaled by a deterministic +/- variance
// drawn from `roll01` in [0,1). Deterministic so a host and client (or a replay)
// agree given the same seed sequence.
inline f32 SwingDamage(const CombatParams& p, f32 roll01) {
  const f32 t = (roll01 * 2.0f - 1.0f) * p.damage_variance;  // [-variance, +variance]
  f32 d = p.base_damage * (1.0f + t);
  return d < 1.0f ? 1.0f : d;
}

// Guest -> main combat channel. The bindings push enrollment/disengage/death on
// the guest thread; the engine drains it on the main thread into the combat
// driver. Mirrors world::WorldCommandQueue.
enum class CombatOp : u8 {
  kEngage,       // actor begins fighting target
  kDisengage,    // actor stops fighting (StopCombat)
  kDied,         // actor died: drop it and anyone targeting it
  kResurrected,  // actor brought back (Reset/Resurrect): clear its downed state
};

struct CombatEvent {
  CombatOp op = CombatOp::kEngage;
  u64 actor = 0;
  u64 target = 0;
};

class CombatEventQueue {
 public:
  void Push(const CombatEvent& e) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.push_back(e);
  }
  std::vector<CombatEvent> Drain() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CombatEvent> out;
    out.swap(pending_);
    return out;
  }

 private:
  std::mutex mutex_;
  std::vector<CombatEvent> pending_;
};

}  // namespace rec::world

#endif  // RECREATION_WORLD_COMBAT_H_
