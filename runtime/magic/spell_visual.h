#ifndef RECREATION_RUNTIME_MAGIC_SPELL_VISUAL_H_
#define RECREATION_RUNTIME_MAGIC_SPELL_VISUAL_H_

#include <base/strings/string_ref.h>

#include "core/types.h"

namespace rx::magic {

// What a spell looks like, one recipe per damage type / school.
//
// The original engine drew every spell through a single effect shader: source
// texture * emissive colour * vertex colour, optionally remapped through a
// greyscale-to-palette texture, blended additively or by alpha, with a
// view-angle falloff and a scrolling uv. Fire and frost differed by their
// TEXTURE and palette, not by their code — which is why there is no per-spell
// shader to port.
//
// This keeps that structure (a palette drives the colour, the blend mode
// separates glow from mist) and drops the parts that only existed because the
// hardware was from 2011:
//   - colour is HDR and emissive, so the bloom pass picks the core up instead
//     of the shader faking a glow with a second quad;
//   - the palette is walked over the particle's LIFE (hot core cooling to
//     smoke) rather than sampled from a static ramp texture;
//   - each cast emits a real dynamic light with its own flicker profile, so
//     spells light the room (and cast ray-traced shadows) instead of being
//     unlit sprites;
//   - motion is buoyancy + drag + turbulence, not a fixed velocity, so flames
//     rise and curl and frost falls.
enum class SpellArchetype : u8 {
  kFire,
  kFrost,
  kShock,
  kRestoration,
  kIllusion,
  kConjuration,
  kAlteration,
  kPoison,
  kDrain,
  kGeneric,
  kCount,
};

struct SpellVisual {
  // Palette walked over a particle's life: core at birth, edge at death.
  f32 core_color[3] = {1, 1, 1};
  f32 edge_color[3] = {0.2f, 0.2f, 0.2f};
  f32 emissive = 1.0f;  // HDR multiplier on the core; > 1 blooms

  f32 light_color[3] = {1, 1, 1};
  f32 light_intensity = 0;  // 0 = the archetype emits no light
  f32 light_radius = 8.0f;
  f32 flicker_hz = 0;
  f32 flicker_depth = 0;  // 0 steady, 1 full strobe

  f32 spawn_rate = 200.0f;  // particles/second while channelling
  f32 life = 0.8f;
  f32 size = 0.10f;
  f32 spread = 0.6f;      // launch cone half-angle, radians
  f32 speed = 1.5f;
  f32 buoyancy = 0;       // + rises (flame), - sinks (frost, poison)
  f32 drag = 1.5f;
  f32 turbulence = 0;     // curl strength

  bool additive = true;   // additive glow (fire, shock) vs alpha mist (frost)
  bool smoke_tail = false;
};

const SpellVisual& VisualFor(SpellArchetype archetype);
const char* SpellArchetypeName(SpellArchetype archetype);

// Picks the archetype from a magic effect's record text. MGEF carries the
// damage type in its editor id and keywords rather than in a single tidy enum,
// so this matches on both; unknown effects fall back to kGeneric rather than
// guessing a school.
SpellArchetype ClassifySpell(base::StringRef editor_id, base::StringRef keyword);

}  // namespace rx::magic

#endif  // RECREATION_RUNTIME_MAGIC_SPELL_VISUAL_H_
