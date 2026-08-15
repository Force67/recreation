#ifndef RECREATION_RUNTIME_MAGIC_SPELL_VFX_H_
#define RECREATION_RUNTIME_MAGIC_SPELL_VFX_H_

#include <base/containers/vector.h>

#include "core/math.h"
#include "render/core/renderer.h"
#include "runtime/magic/spell_visual.h"

namespace rx::magic {

// How far along a cast is. The original game split a spell's art the same way
// (casting art at the hand, a projectile, then hit art), so the phases are what
// a spell record already describes; only the look is rebuilt.
enum class CastPhase : u8 {
  kCharge,      // gathering at the caster's hand: tight, inward, growing
  kProjectile,  // in flight: a moving core with a trail
  kImpact,      // a one-shot burst that decays on its own
};

// Live spell effects: particle pools plus one dynamic light per cast, stepped
// on the CPU and appended to the frame view. No GPU resources of its own, so
// it can be created and destroyed freely.
class SpellVfx {
 public:
  // Starts an effect at `position`. Charge and projectile keep emitting until
  // End(); impact is one burst and needs no End. Returns a handle, or 0 if the
  // budget is full.
  u32 Begin(SpellArchetype archetype, CastPhase phase, const Vec3& position);

  // Moves a live cast (a projectile tracking its flight, a charge following the
  // hand). Ignored for handles that have already finished.
  void Move(u32 cast, const Vec3& position);

  // Stops emission; the particles already alive finish their life so the effect
  // fades instead of popping.
  void End(u32 cast);

  void Step(f32 dt);
  void EmitToView(render::FrameView& view) const;

  u32 live_casts() const { return static_cast<u32>(casts_.size()); }
  u32 live_particles() const { return static_cast<u32>(particles_.size()); }

  // Whole-system particle budget, shared across casts.
  static constexpr u32 kMaxParticles = 24000;

 private:
  struct Particle {
    Vec3 position{};
    Vec3 prev{};
    Vec3 velocity{};
    f32 age = 0;
    f32 life = 1;
    f32 size = 0.1f;
    f32 phase = 0;  // per-particle turbulence offset, keeps them from moving as one
    u32 cast = 0;
  };

  struct Cast {
    u32 handle = 0;
    SpellArchetype archetype = SpellArchetype::kGeneric;
    CastPhase phase = CastPhase::kCharge;
    Vec3 position{};
    f32 time = 0;
    f32 spawn_accum = 0;
    f32 strength = 1.0f;  // 1 while emitting, decays once ended
    bool emitting = true;
  };

  f32 Random();
  Cast* Find(u32 handle);
  const Cast* Find(u32 handle) const;
  void Spawn(const Cast& cast, const SpellVisual& visual, u32 count);

  base::Vector<Particle> particles_;
  base::Vector<Cast> casts_;
  u32 next_handle_ = 1;
  u32 seed_ = 0x9e3779b9u;
};

}  // namespace rx::magic

#endif  // RECREATION_RUNTIME_MAGIC_SPELL_VFX_H_
