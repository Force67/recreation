// spell_vfxtest: checks the spell effect recipes and the pool simulation.
// Pure CPU state, so it runs without a window, a device or game data.

#include <cstdio>

#include "runtime/magic/spell_vfx.h"
#include "runtime/magic/spell_visual.h"

namespace {

using rx::f32;
using rx::u32;
using rx::Vec3;
using rx::magic::CastPhase;
using rx::magic::SpellArchetype;
using rx::magic::SpellVfx;

int g_failures = 0;
void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    ++g_failures;
}

// Steps a system for `seconds` at a fixed tick.
void Run(SpellVfx& vfx, f32 seconds) {
  const f32 dt = 1.0f / 60.0f;
  for (f32 t = 0; t < seconds; t += dt)
    vfx.Step(dt);
}

f32 MeanHeight(const rx::render::FrameView& view) {
  if (view.particles.empty())
    return 0;
  f32 sum = 0;
  for (const auto& p : view.particles)
    sum += p.pos[1];
  return sum / static_cast<f32>(view.particles.size());
}

void TestClassification() {
  std::puts("classification");
  using rx::magic::ClassifySpell;

  Check("firebolt is fire", ClassifySpell("FireboltDamage", "") == SpellArchetype::kFire);
  Check("ice spike is frost", ClassifySpell("IceSpikeDamage", "") == SpellArchetype::kFrost);
  Check("sparks is shock", ClassifySpell("SparksDamage", "") == SpellArchetype::kShock);
  Check("healing is restoration", ClassifySpell("HealingSelf", "") == SpellArchetype::kRestoration);
  Check("conjure is conjuration",
        ClassifySpell("SummonFamiliar", "") == SpellArchetype::kConjuration);
  Check("oakflesh is alteration", ClassifySpell("OakfleshSelf", "") == SpellArchetype::kAlteration);
  Check("fear is illusion", ClassifySpell("FearFFSelf", "") == SpellArchetype::kIllusion);
  Check("case insensitive", ClassifySpell("FIREBALLAOE", "") == SpellArchetype::kFire);
  // "Absorb Health" restores the caster but must not read as a heal: it is a
  // drain, and drains look nothing like restoration.
  Check("absorb health is drain", ClassifySpell("AbsorbHealth", "") == SpellArchetype::kDrain);
  Check("poison is poison", ClassifySpell("PoisonDamage", "") == SpellArchetype::kPoison);
  // The keyword wins when the two disagree, since MagicDamageFire is explicit
  // where an editor id is only a naming convention.
  Check("keyword beats editor id",
        ClassifySpell("DunFolgunthurPuzzle", "MagicDamageFire") == SpellArchetype::kFire);
  Check("unknown falls back to generic",
        ClassifySpell("DunTrapLever01", "") == SpellArchetype::kGeneric);
}

void TestRecipes() {
  std::puts("recipes");
  bool all_usable = true;
  for (u32 i = 0; i < static_cast<u32>(SpellArchetype::kCount); ++i) {
    const auto& v = rx::magic::VisualFor(static_cast<SpellArchetype>(i));
    if (v.life <= 0 || v.spawn_rate <= 0 || v.size <= 0 || v.emissive <= 0)
      all_usable = false;
  }
  Check("every archetype has a usable recipe", all_usable);

  const auto& fire = rx::magic::VisualFor(SpellArchetype::kFire);
  const auto& frost = rx::magic::VisualFor(SpellArchetype::kFrost);
  const auto& shock = rx::magic::VisualFor(SpellArchetype::kShock);
  Check("fire rises", fire.buoyancy > 0);
  Check("frost sinks", frost.buoyancy < 0);
  Check("fire glows additively", fire.additive);
  Check("frost is alpha mist", !frost.additive);
  Check("shock strobes hardest", shock.flicker_depth > fire.flicker_depth);
  Check("shock is the brightest light", shock.light_intensity > fire.light_intensity);
  Check("only fire trails smoke", fire.smoke_tail && !frost.smoke_tail && !shock.smoke_tail);
  Check("name lookup", base::StringRef(rx::magic::SpellArchetypeName(SpellArchetype::kFrost)) ==
                           "frost");
}

void TestChannelling() {
  std::puts("channelled cast");
  SpellVfx vfx;
  const u32 cast = vfx.Begin(SpellArchetype::kFire, CastPhase::kProjectile, {0, 10, 0});
  Check("handle issued", cast != 0);
  Check("one live cast", vfx.live_casts() == 1);

  Run(vfx, 0.5f);
  Check("emits while channelling", vfx.live_particles() > 0);

  // Ending stops emission but must not cut the effect off mid-air.
  vfx.End(cast);
  const u32 at_release = vfx.live_particles();
  vfx.Step(1.0f / 60.0f);
  Check("no pop on release", vfx.live_particles() <= at_release && vfx.live_particles() > 0);

  Run(vfx, 4.0f);
  Check("drains to empty", vfx.live_particles() == 0);
  Check("cast retired", vfx.live_casts() == 0);
}

void TestImpact() {
  std::puts("impact burst");
  SpellVfx vfx;
  vfx.Begin(SpellArchetype::kShock, CastPhase::kImpact, {0, 2, 0});
  const u32 burst = vfx.live_particles();
  Check("bursts immediately", burst > 0);

  Run(vfx, 0.2f);
  Check("does not keep emitting", vfx.live_particles() <= burst);
  Run(vfx, 5.0f);
  Check("decays away", vfx.live_particles() == 0 && vfx.live_casts() == 0);
}

void TestMotionAndOutput() {
  std::puts("motion and frame output");

  SpellVfx fire;
  fire.Begin(SpellArchetype::kFire, CastPhase::kProjectile, {0, 0, 0});
  Run(fire, 0.6f);
  rx::render::FrameView fire_view;
  fire.EmitToView(fire_view);

  SpellVfx frost;
  frost.Begin(SpellArchetype::kFrost, CastPhase::kProjectile, {0, 0, 0});
  Run(frost, 0.6f);
  rx::render::FrameView frost_view;
  frost.EmitToView(frost_view);

  Check("fire climbs", MeanHeight(fire_view) > 0.0f);
  Check("frost falls", MeanHeight(frost_view) < MeanHeight(fire_view));
  Check("particles reach the view", !fire_view.particles.empty());
  Check("a cast lights the scene", fire_view.lights.size() == 1);
  Check("light is warm", fire_view.lights[0].color_intensity[0] >
                             fire_view.lights[0].color_intensity[2]);
  Check("light has positive intensity", fire_view.lights[0].color_intensity[3] > 0);

  // Emissive colour above 1 is what makes the bloom pass pick the core up.
  bool any_hdr = false;
  for (const auto& p : fire_view.particles) {
    if (p.color[0] > 1.0f)
      any_hdr = true;
  }
  Check("fire core is hdr", any_hdr);

  // Alpha-blended archetypes must stay inside 0..1 or they blow out the blend.
  bool alpha_sane = true;
  for (const auto& p : frost_view.particles) {
    if (p.color[3] < 0.0f || p.color[3] > 1.0f)
      alpha_sane = false;
  }
  Check("frost alpha in range", alpha_sane);

  // Motion vectors need a real previous position, or TAA smears the sprites.
  bool moved = false;
  for (const auto& p : fire_view.particles) {
    if (p.prev_pos[1] != p.pos[1])
      moved = true;
  }
  Check("motion vectors populated", moved);
}

void TestBudget() {
  std::puts("budget");
  SpellVfx vfx;
  for (u32 i = 0; i < 60; ++i)
    vfx.Begin(SpellArchetype::kFire, CastPhase::kProjectile, {static_cast<f32>(i), 0, 0});
  Run(vfx, 3.0f);
  Check("particle budget held", vfx.live_particles() <= SpellVfx::kMaxParticles);

  // A long hitch must not integrate the pools into orbit.
  SpellVfx hitched;
  hitched.Begin(SpellArchetype::kFire, CastPhase::kProjectile, {0, 0, 0});
  hitched.Step(2.0f);
  rx::render::FrameView view;
  hitched.EmitToView(view);
  bool sane = true;
  for (const auto& p : view.particles) {
    if (p.pos[1] > 100.0f || p.pos[1] < -100.0f)
      sane = false;
  }
  Check("hitch clamped", sane);
}

}  // namespace

int main() {
  std::puts("spell_vfxtest");
  TestClassification();
  TestRecipes();
  TestChannelling();
  TestImpact();
  TestMotionAndOutput();
  TestBudget();

  if (g_failures == 0) {
    std::puts("spell_vfx: all checks passed");
    return 0;
  }
  std::printf("spell_vfx: %d checks FAILED\n", g_failures);
  return 1;
}
