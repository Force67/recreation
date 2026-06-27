// combattest: the pure melee-combat geometry (world/combat.h) and the bindings'
// combat state machine (StartCombat -> ApplyMeleeHit -> death), including kill
// attribution and the combat-disengage-on-death cleanup. No game data needed.

#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

#include "quest/quest_def.h"
#include "script/games/skyrim/skyrim_bindings.h"
#include "script/host/bridge.h"
#include "script/world_effect_sink.h"
#include "world/combat.h"

using rec::script::papyrus::ObjectRef;
using rec::script::skyrim::RecordBackedSkyrimBindings;

namespace {

int g_failures = 0;
void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}
bool Near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) < eps; }

// Records the death notifications routed to the world sink, so we can assert the
// main-thread driver would be told to drop the dead actor.
class RecordingSink : public rec::script::WorldEffectSink {
 public:
  rec::u64 SpawnReference(rec::u64, rec::u64, float, float, float) override { return 0; }
  void MoveReference(rec::u64, rec::u64, float, float, float) override {}
  void MovePlayer(rec::u64, rec::u64, float, float, float) override {}
  void SetEnabled(rec::u64, rec::u64, bool) override {}
  void DeleteReference(rec::u64, rec::u64) override {}
  void CleanupQuest(rec::u64) override {}
  void StartCombat(rec::u64, rec::u64 a, rec::u64 t) override { engaged.push_back({a, t}); }
  void StopCombat(rec::u64, rec::u64 a) override { disengaged.push_back(a); }
  void ActorDied(rec::u64, rec::u64 a) override { died.push_back(a); }
  std::vector<std::pair<rec::u64, rec::u64>> engaged;
  std::vector<rec::u64> disengaged;
  std::vector<rec::u64> died;
};

void TestPureHelpers() {
  std::puts("pure combat geometry:");
  using namespace rec::world;
  const float a[3] = {0, 0, 0};
  const float b[3] = {3, 100, 4};  // height ignored: planar distance is 5
  Check("planar dist ignores height", Near(PlanarDist2(a, b), 25.0f));
  Check("within reach (5 <= 6)", WithinPlanar(a, b, 6.0f));
  Check("out of reach (5 > 4)", !WithinPlanar(a, b, 4.0f));

  const float pts[9] = {10, 0, 0, /*idx1*/ 2, 0, 2, /*idx2*/ -8, 0, 0};
  Check("nearest within radius picks closest", NearestWithin(a, pts, 3, 20.0f) == 1);
  Check("nearest beyond radius -> none", NearestWithin(a, pts, 3, 1.0f) == -1);

  // Melee arc: a forward swing (facing -Z) hits what is ahead, not behind/beside.
  const float face[3] = {0, 0, -1};  // unit forward, -Z
  const float ahead[3] = {0, 0, -2};
  const float behind[3] = {0, 0, 2};
  const float beside[3] = {2, 0, 0};
  const float far_ahead[3] = {0, 0, -9};
  Check("strikes a target dead ahead", InMeleeArc(a, ahead, face, 3.0f, 0.35f));
  Check("misses a target behind", !InMeleeArc(a, behind, face, 3.0f, 0.35f));
  Check("misses a target to the side (outside arc)", !InMeleeArc(a, beside, face, 3.0f, 0.35f));
  Check("misses a target out of reach", !InMeleeArc(a, far_ahead, face, 3.0f, 0.35f));

  CombatParams p;
  Check("swing damage at mid roll == base", Near(SwingDamage(p, 0.5f), p.base_damage, 0.5f));
  Check("swing damage min roll < base", SwingDamage(p, 0.0f) < p.base_damage);
  Check("swing damage max roll > base", SwingDamage(p, 0.999f) > p.base_damage);
  Check("swing damage never below 1", SwingDamage(p, 0.0f) >= 1.0f);

  CombatEventQueue q;
  q.Push({CombatOp::kEngage, 1, 2});
  q.Push({CombatOp::kDied, 2, 0});
  auto drained = q.Drain();
  Check("queue drains in order", drained.size() == 2 && drained[0].actor == 1 &&
                                     drained[1].op == CombatOp::kDied);
  Check("queue empty after drain", q.Drain().empty());
}

void TestBindingsCombat() {
  std::puts("bindings combat + death:");
  RecordBackedSkyrimBindings binds;
  RecordingSink sink;
  binds.set_world_sink(&sink);

  rec::u64 killer = 0;
  binds.set_event_sink([&](const rec::script::host::ManagedEvent& e) {
    if (e.id == rec::script::host::ManagedEventId::kActorDied) killer = e.b;
  });

  const ObjectRef soldier{0x100};
  const ObjectRef enemy{0x200};
  binds.SetActorValue(enemy, "health", 50.0f);

  binds.StartCombat(soldier, enemy);
  Check("attacker is in combat", binds.IsInCombat(soldier));
  Check("combat target is the enemy", binds.GetCombatTarget(soldier).handle == enemy.handle);
  Check("StartCombat routed to sink", sink.engaged.size() == 1 &&
                                          sink.engaged[0].second == enemy.handle);

  binds.ApplyMeleeHit(soldier, enemy, 30.0f);
  Check("enemy survives first hit", !binds.IsDead(enemy));
  Check("enemy health reduced", Near(binds.GetActorValue(enemy, "health"), 20.0f, 0.01f));

  binds.ApplyMeleeHit(soldier, enemy, 30.0f);
  Check("enemy dies on lethal hit", binds.IsDead(enemy));
  Check("death attributed to the attacker", killer == soldier.handle);
  Check("death routed to sink", sink.died.size() == 1 && sink.died[0] == enemy.handle);
  Check("attacker disengaged when target died", !binds.IsInCombat(soldier));

  // A swing on an already-dead target is a no-op (no double OnDeath).
  killer = 0;
  binds.ApplyMeleeHit(soldier, enemy, 30.0f);
  Check("no re-kill of a corpse", killer == 0 && sink.died.size() == 1);
}

void TestTokenResolution() {
  std::puts("quest text tokens:");
  RecordBackedSkyrimBindings binds;  // no records: globals stay, aliases fall back to name
  rec::quest::QuestDef def;
  def.handle = 0xCAFE;
  rec::quest::AliasDef city;
  city.id = 1;
  city.name = "City";
  def.aliases.push_back(city);
  binds.quest_system().SetDefinition(def);

  Check("alias token falls back to its name when unfilled",
        binds.ResolveQuestText(0xCAFE, "Battle for <Alias=City>") == "Battle for Whiterun" ||
            binds.ResolveQuestText(0xCAFE, "Battle for <Alias=City>") == "Battle for City");
  Check("unknown token left in place",
        binds.ResolveQuestText(0xCAFE, "x <Foo=bar> y") == "x <Foo=bar> y");
  Check("unresolved global token left in place",
        binds.ResolveQuestText(0xCAFE, "<Global=Nope>") == "<Global=Nope>");
  Check("plain text unchanged", binds.ResolveQuestText(0xCAFE, "plain text") == "plain text");
}

}  // namespace

int main() {
  TestPureHelpers();
  TestBindingsCombat();
  TestTokenResolution();
  std::printf("%s\n", g_failures ? "COMBATTEST FAILED" : "COMBATTEST PASSED");
  return g_failures ? 1 : 0;
}
