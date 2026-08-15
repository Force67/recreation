#include "runtime/magic/spell_vfx.h"

#include <cmath>

namespace rx::magic {
namespace {

// An impact is a single burst, sized so a hit reads as a hit without the pool
// budget disappearing into one explosion.
constexpr u32 kImpactBurst = 900;
// How fast an ended cast's light fades out, in strength per second.
constexpr f32 kReleaseFade = 3.5f;

f32 Lerp(f32 a, f32 b, f32 t) {
  return a + (b - a) * t;
}

}  // namespace

f32 SpellVfx::Random() {
  seed_ ^= seed_ << 13;
  seed_ ^= seed_ >> 17;
  seed_ ^= seed_ << 5;
  return static_cast<f32>(seed_ & 0xffffffu) / 16777216.0f;
}

SpellVfx::Cast* SpellVfx::Find(u32 handle) {
  for (Cast& c : casts_) {
    if (c.handle == handle)
      return &c;
  }
  return nullptr;
}

const SpellVfx::Cast* SpellVfx::Find(u32 handle) const {
  for (const Cast& c : casts_) {
    if (c.handle == handle)
      return &c;
  }
  return nullptr;
}

void SpellVfx::Spawn(const Cast& cast, const SpellVisual& visual, u32 count) {
  for (u32 i = 0; i < count; ++i) {
    if (particles_.size() >= kMaxParticles)
      return;

    Particle p;
    p.cast = cast.handle;
    p.phase = Random() * 6.2831853f;

    // Direction: a cone about +y, widened by the recipe's spread. A charge
    // pulls inward instead, so it gathers at the hand rather than spraying.
    const f32 azimuth = Random() * 6.2831853f;
    const f32 tilt = Random() * visual.spread;
    const Vec3 dir{std::sin(tilt) * std::cos(azimuth), std::cos(tilt),
                   std::sin(tilt) * std::sin(azimuth)};

    f32 speed = visual.speed * (0.6f + 0.8f * Random());
    if (cast.phase == CastPhase::kImpact)
      speed *= 2.4f;  // a hit throws its debris out hard

    if (cast.phase == CastPhase::kCharge) {
      // Start on a small shell and fall inward: the gather that precedes a
      // release, which the old art faked with a shrinking billboard.
      const f32 radius = 0.22f + 0.12f * Random();
      p.position = {cast.position.x + dir.x * radius, cast.position.y + dir.y * radius * 0.6f,
                    cast.position.z + dir.z * radius};
      p.velocity = {-dir.x * speed * 0.5f, -dir.y * speed * 0.2f, -dir.z * speed * 0.5f};
    } else {
      p.position = cast.position;
      p.velocity = {dir.x * speed, dir.y * speed, dir.z * speed};
    }

    p.prev = p.position;
    p.life = visual.life * (0.7f + 0.6f * Random());
    p.age = 0;
    p.size = visual.size * (0.7f + 0.7f * Random());
    particles_.push_back(p);
  }
}

u32 SpellVfx::Begin(SpellArchetype archetype, CastPhase phase, const Vec3& position) {
  if (particles_.size() >= kMaxParticles)
    return 0;

  Cast cast;
  cast.handle = next_handle_++;
  cast.archetype = archetype;
  cast.phase = phase;
  cast.position = position;
  // An impact never emits again; it is the burst below and then a fade.
  cast.emitting = phase != CastPhase::kImpact;
  casts_.push_back(cast);

  if (phase == CastPhase::kImpact)
    Spawn(cast, VisualFor(archetype), kImpactBurst);
  return cast.handle;
}

void SpellVfx::Move(u32 cast, const Vec3& position) {
  if (Cast* c = Find(cast))
    c->position = position;
}

void SpellVfx::End(u32 cast) {
  if (Cast* c = Find(cast))
    c->emitting = false;
}

void SpellVfx::Step(f32 dt) {
  if (dt <= 0)
    return;
  if (dt > 0.05f)
    dt = 0.05f;  // a hitch must not blow the pools open

  // Emit, and age out the casts whose particles have all gone.
  for (size_t i = 0; i < casts_.size();) {
    Cast& cast = casts_[i];
    const SpellVisual& visual = VisualFor(cast.archetype);
    cast.time += dt;

    if (cast.emitting) {
      cast.spawn_accum += visual.spawn_rate * dt;
      const u32 count = static_cast<u32>(cast.spawn_accum);
      cast.spawn_accum -= static_cast<f32>(count);
      if (count != 0)
        Spawn(cast, visual, count);
    } else {
      cast.strength -= kReleaseFade * dt;
      if (cast.strength < 0)
        cast.strength = 0;
    }

    // A finished cast sticks around until its last particle dies, so the light
    // fades with the embers instead of cutting out under them.
    if (!cast.emitting && cast.strength <= 0.0f) {
      bool alive = false;
      for (const Particle& p : particles_) {
        if (p.cast == cast.handle) {
          alive = true;
          break;
        }
      }
      if (!alive) {
        casts_[i] = casts_.back();
        casts_.pop_back();
        continue;
      }
    }
    ++i;
  }

  // Integrate.
  for (size_t i = 0; i < particles_.size();) {
    Particle& p = particles_[i];
    p.age += dt;
    if (p.age >= p.life) {
      particles_[i] = particles_.back();
      particles_.pop_back();
      continue;
    }

    const Cast* cast = Find(p.cast);
    const SpellVisual& visual =
        VisualFor(cast ? cast->archetype : SpellArchetype::kGeneric);

    // Buoyancy (flames climb, frost and poison sink) plus a cheap curl so the
    // pool does not travel as one rigid cone.
    p.velocity.y += visual.buoyancy * dt;
    if (visual.turbulence > 0) {
      const f32 t = p.age * 3.1f + p.phase;
      p.velocity.x += std::sin(t * 1.7f) * visual.turbulence * dt;
      p.velocity.y += std::sin(t * 2.3f + 1.1f) * visual.turbulence * 0.4f * dt;
      p.velocity.z += std::cos(t * 1.9f + 0.7f) * visual.turbulence * dt;
    }

    const f32 drag = 1.0f - visual.drag * dt;
    const f32 damp = drag < 0 ? 0 : drag;
    p.velocity.x *= damp;
    p.velocity.y *= damp;
    p.velocity.z *= damp;

    p.prev = p.position;
    p.position.x += p.velocity.x * dt;
    p.position.y += p.velocity.y * dt;
    p.position.z += p.velocity.z * dt;
    ++i;
  }
}

void SpellVfx::EmitToView(render::FrameView& view) const {
  view.particles.reserve(view.particles.size() + particles_.size());
  for (const Particle& p : particles_) {
    const Cast* cast = Find(p.cast);
    const SpellVisual& visual =
        VisualFor(cast ? cast->archetype : SpellArchetype::kGeneric);

    // Walk the palette over the particle's life: this is the greyscale-to-
    // palette remap the original shader did against a ramp texture, except the
    // ramp is time rather than source luminance, so a flame cools as it climbs.
    const f32 t = p.age / p.life;  // 0 at birth, 1 at death
    const f32 heat = 1.0f - t;

    render::ParticleInstance inst;
    inst.pos[0] = p.position.x;
    inst.pos[1] = p.position.y;
    inst.pos[2] = p.position.z;
    inst.prev_pos[0] = p.prev.x;
    inst.prev_pos[1] = p.prev.y;
    inst.prev_pos[2] = p.prev.z;
    // Embers shrink, mist and smoke swell.
    inst.size = p.size * (visual.smoke_tail ? Lerp(1.0f, 2.1f, t) : Lerp(1.15f, 0.75f, t));

    // Emissive scales with the remaining heat, so only the young core is bright
    // enough to bloom.
    const f32 emissive = visual.additive ? visual.emissive * heat * heat : 1.0f;
    for (u32 c = 0; c < 3; ++c)
      inst.color[c] = Lerp(visual.edge_color[c], visual.core_color[c], heat) * emissive;

    // Additive glow rides its colour and fades to nothing; alpha mist holds
    // opacity through the middle of its life and then thins out.
    inst.color[3] = visual.additive ? heat * heat : 0.55f * heat * (0.4f + 0.6f * heat);
    view.particles.push_back(inst);
  }

  for (const Cast& cast : casts_) {
    const SpellVisual& visual = VisualFor(cast.archetype);
    if (visual.light_intensity <= 0)
      continue;

    // Per-archetype flicker: layered sines for the organic ones, a hard strobe
    // for shock. The old effect shader was unlit, so none of this existed —
    // spells never lit the room they were cast in.
    f32 flicker = 1.0f;
    if (visual.flicker_hz > 0 && visual.flicker_depth > 0) {
      const f32 t = cast.time * visual.flicker_hz;
      f32 wave = 0.6f * std::sin(t) + 0.3f * std::sin(t * 2.3f + 1.7f) +
                 0.1f * std::sin(t * 0.7f + 0.6f);
      if (visual.flicker_depth > 0.4f)  // shock: snap rather than breathe
        wave = wave > 0 ? 1.0f : -1.0f;
      flicker = 1.0f + visual.flicker_depth * wave;
      if (flicker < 0)
        flicker = 0;
    }

    // An impact flashes bright and collapses; a channelled spell holds.
    f32 amplitude = cast.strength;
    if (cast.phase == CastPhase::kImpact)
      amplitude = cast.strength * (0.35f + 0.65f * flicker);

    render::PointLight light;
    light.pos_radius[0] = cast.position.x;
    light.pos_radius[1] = cast.position.y;
    light.pos_radius[2] = cast.position.z;
    light.pos_radius[3] = visual.light_radius;
    light.color_intensity[0] = visual.light_color[0];
    light.color_intensity[1] = visual.light_color[1];
    light.color_intensity[2] = visual.light_color[2];
    light.color_intensity[3] = visual.light_intensity * flicker * amplitude;
    view.lights.push_back(light);
  }
}

}  // namespace rx::magic
