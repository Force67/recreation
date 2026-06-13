// bindingstest: validate the record-backed Skyrim bindings against real assets.
//
//   bindingstest <data_dir>
//
// Checks the new actor-value and inventory systems for correct stateful
// behavior, and the record-backed form natives (type, keywords) against a real
// weapon record pulled from the loaded plugins.

#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

#include "bethesda/load_order.h"
#include "bethesda/record.h"
#include "script/games/skyrim/skyrim_bindings.h"

namespace {

using namespace rec;
using namespace rec::bethesda;
using rec::script::papyrus::ObjectRef;
using rec::script::skyrim::RecordBackedSkyrimBindings;

rec::u64 Handle(GlobalFormId id) { return id.packed(); }

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <data_dir>\n", argv[0]);
    return 2;
  }
  std::string data_dir = argv[1];
  const auto& profile = GameProfile::For(GameProfile::DetectFromDataDir(data_dir));
  auto order = LoadOrder::FromPluginsTxt(data_dir + "/../plugins.txt", profile);
  RecordStore records;
  if (!records.LoadAll(data_dir, order, profile)) {
    std::printf("failed to load records\n");
    return 1;
  }

  RecordBackedSkyrimBindings bindings(&records);

  int failures = 0;
  auto check = [&](const char* what, bool ok) {
    std::printf("  %-48s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
  };

  // Actor values (new system): defaults, set, mod, death.
  ObjectRef actor{0x14};
  check("default Health is 100", bindings.GetActorValue(actor, "Health") == 100.0f);
  bindings.SetActorValue(actor, "Health", 80.0f);
  check("set Health -> 80", bindings.GetActorValue(actor, "Health") == 80.0f);
  bindings.ModActorValue(actor, "Health", -25.0f);
  check("damage 25 -> current 55", bindings.GetActorValue(actor, "Health") == 55.0f);
  check("base Health still 80", bindings.GetBaseActorValue(actor, "Health") == 80.0f);
  check("Health percentage 55/80", bindings.GetActorValuePercentage(actor, "Health") == 0.6875f);
  check("not dead at 55", !bindings.IsDead(actor));
  bindings.RestoreActorValue(actor, "Health", 1000.0f);
  check("restore caps at base 80", bindings.GetActorValue(actor, "Health") == 80.0f);
  bindings.ModActorValue(actor, "Health", -100.0f);
  check("dead after lethal damage", bindings.IsDead(actor));

  // Inventory (new system).
  ObjectRef chest{0x100};
  ObjectRef gold{0xF};
  check("empty count is 0", bindings.GetItemCount(chest, gold) == 0);
  bindings.AddItem(chest, gold, 100);
  bindings.AddItem(chest, gold, 50);
  check("add 100 + 50 -> 150", bindings.GetItemCount(chest, gold) == 150);
  bindings.RemoveItem(chest, gold, 60);
  check("remove 60 -> 90", bindings.GetItemCount(chest, gold) == 90);
  bindings.RemoveItem(chest, gold, 1000);
  check("over-remove clamps to 0", bindings.GetItemCount(chest, gold) == 0);

  // Form data from the real RecordStore: first weapon's GetType and a keyword.
  std::optional<GlobalFormId> weapon;
  u32 weapon_keyword = 0;
  records.EachOfType(FourCc('W', 'E', 'A', 'P'), [&](GlobalFormId id, const RecordStore::StoredRecord&) {
    if (weapon) return;
    Record rec;
    if (!records.Parse(id, &rec)) return;
    const Subrecord* kwda = rec.Find(FourCc('K', 'W', 'D', 'A'));
    if (!kwda || kwda->data.size() < 4) return;  // pick one with a keyword to test
    weapon = id;
    std::memcpy(&weapon_keyword, kwda->data.data(), 4);
  });

  if (weapon) {
    ObjectRef w{Handle(*weapon)};
    check("real WEAP GetType == 42", bindings.GetFormType(w) == 42);
    check("real WEAP has its first keyword",
          bindings.HasKeyword(w, ObjectRef{weapon_keyword}));
    check("real WEAP lacks bogus keyword", !bindings.HasKeyword(w, ObjectRef{0xBADF00D}));
    std::printf("  (weapon %04x:%06x, GetType=%d)\n", weapon->plugin, weapon->local_id,
                bindings.GetFormType(w));
  } else {
    std::printf("  (no keyworded weapon found to test form natives)\n");
  }

  // Game.GetForm round-trips a real weapon's runtime form id back to its handle.
  if (weapon) {
    u32 runtime_id = (static_cast<u32>(weapon->plugin) << 24) | weapon->local_id;
    ObjectRef looked_up = bindings.GetForm(runtime_id);
    check("Game.GetForm resolves to the same handle", looked_up.handle == Handle(*weapon));
    check("Game.GetForm of a missing id is None", bindings.GetForm(0x00FFFFFE).handle == 0);
  }

  // ActorBase data from a real NPC_ record.
  std::optional<GlobalFormId> npc_base;
  records.EachOfType(FourCc('N', 'P', 'C', '_'), [&](GlobalFormId id, const RecordStore::StoredRecord&) {
    if (npc_base) return;
    Record rec;
    if (records.Parse(id, &rec) && rec.Find(FourCc('R', 'N', 'A', 'M'))) npc_base = id;
  });
  if (npc_base) {
    ObjectRef base{Handle(*npc_base)};
    i32 sex = bindings.GetSex(base);
    check("NPC GetSex is 0 or 1", sex == 0 || sex == 1);
    check("NPC GetRace returns a form", bindings.GetRace(base).handle != 0);
    std::printf("  (npc %04x:%06x, sex=%d, race=%llx, essential=%d)\n", npc_base->plugin,
                npc_base->local_id, sex,
                (unsigned long long)bindings.GetRace(base).handle, bindings.IsEssential(base));
  }

  // Spatial natives (records-authored placement + override store).
  std::optional<GlobalFormId> ref;
  f32 authored[3] = {0, 0, 0};
  records.EachOfType(FourCc('R', 'E', 'F', 'R'), [&](GlobalFormId id, const RecordStore::StoredRecord&) {
    if (ref) return;
    Record rec;
    if (!records.Parse(id, &rec)) return;
    const Subrecord* data = rec.Find(FourCc('D', 'A', 'T', 'A'));
    if (!data || data->data.size() < 12) return;
    ref = id;
    std::memcpy(authored, data->data.data(), 12);
  });
  if (ref) {
    ObjectRef r{Handle(*ref)};
    check("REFR GetPositionX matches authored DATA", bindings.GetPositionX(r) == authored[0]);
    bindings.SetPosition(r, 1.0f, 2.0f, 3.0f);
    check("SetPosition overrides X", bindings.GetPositionX(r) == 1.0f);
    check("SetPosition overrides Z", bindings.GetPositionZ(r) == 3.0f);
    ObjectRef other{0x999};
    bindings.SetPosition(other, 1.0f, 2.0f, 0.0f);  // 3 units away on Z from r
    check("GetDistance is 3", bindings.GetDistance(r, other) == 3.0f);
    bindings.MoveTo(r, other);
    check("MoveTo snaps to target", bindings.GetDistance(r, other) == 0.0f);
  }

  // Record-backed object queries: GetBaseObject and Cell.IsInterior.
  std::optional<GlobalFormId> placed;
  records.EachOfType(FourCc('R', 'E', 'F', 'R'), [&](GlobalFormId id, const RecordStore::StoredRecord&) {
    if (placed) return;
    Record rec;
    if (records.Parse(id, &rec) && rec.Find(FourCc('N', 'A', 'M', 'E'))) placed = id;
  });
  if (placed)
    check("REFR GetBaseObject resolves a base form",
          bindings.GetBaseObject(ObjectRef{Handle(*placed)}).handle != 0);

  std::optional<GlobalFormId> cell;
  bool cell_interior = false;
  records.EachOfType(FourCc('C', 'E', 'L', 'L'), [&](GlobalFormId id, const RecordStore::StoredRecord&) {
    if (cell) return;
    Record rec;
    if (!records.Parse(id, &rec)) return;
    const Subrecord* data = rec.Find(FourCc('D', 'A', 'T', 'A'));
    if (!data || data->data.empty()) return;
    cell = id;
    cell_interior = (data->data[0] & 0x1) != 0;
  });
  if (cell)
    check("CELL IsInterior matches DATA flag",
          bindings.IsInterior(ObjectRef{Handle(*cell)}) == cell_interior);

  // GlobalVariable: authored value from GLOB record, then override.
  std::optional<GlobalFormId> glob;
  f32 glob_authored = 0;
  records.EachOfType(FourCc('G', 'L', 'O', 'B'), [&](GlobalFormId id, const RecordStore::StoredRecord&) {
    if (glob) return;
    Record rec;
    if (!records.Parse(id, &rec)) return;
    const Subrecord* fltv = rec.Find(FourCc('F', 'L', 'T', 'V'));
    if (!fltv || fltv->data.size() < 4) return;
    glob = id;
    std::memcpy(&glob_authored, fltv->data.data(), 4);
  });
  if (glob) {
    ObjectRef g{Handle(*glob)};
    check("global value matches authored FLTV", bindings.GetGlobalValue(g) == glob_authored);
    bindings.SetGlobalValue(g, 42.0f);
    check("global value override -> 42", bindings.GetGlobalValue(g) == 42.0f);
  }

  // Quest state (new system).
  ObjectRef quest{0x123};
  check("quest stage default 0", bindings.GetStage(quest) == 0);
  bindings.SetStage(quest, 10);
  check("SetStage 10 -> GetStage 10", bindings.GetStage(quest) == 10);
  check("stage 10 marked done", bindings.GetStageDone(quest, 10));
  check("stage 5 not done", !bindings.GetStageDone(quest, 5));
  check("running after SetStage", bindings.IsRunning(quest));
  bindings.SetObjectiveDisplayed(quest, 1, true);
  bindings.SetObjectiveCompleted(quest, 1, true);
  check("objective 1 displayed", bindings.IsObjectiveDisplayed(quest, 1));
  check("objective 1 completed", bindings.IsObjectiveCompleted(quest, 1));
  bindings.StopQuest(quest);
  check("stopped quest not running", !bindings.IsRunning(quest));

  // Factions (new system) and scale.
  ObjectRef npc{0x1A};
  ObjectRef thieves{0x2BUL};
  check("not in faction -> rank -2", bindings.GetFactionRank(npc, thieves) == -2);
  bindings.AddToFaction(npc, thieves);
  check("added to faction", bindings.IsInFaction(npc, thieves));
  bindings.SetFactionRank(npc, thieves, 3);
  check("faction rank set to 3", bindings.GetFactionRank(npc, thieves) == 3);
  bindings.RemoveFromFaction(npc, thieves);
  check("removed from faction", !bindings.IsInFaction(npc, thieves));
  bindings.SetCrimeGold(thieves, 100);
  bindings.ModCrimeGold(thieves, 50);
  check("crime gold 100 + 50 = 150", bindings.GetCrimeGold(thieves) == 150);
  ObjectRef rock{0x3C};
  check("default scale is 1.0", bindings.GetScale(rock) == 1.0f);
  bindings.SetScale(rock, 2.5f);
  check("scale set to 2.5", bindings.GetScale(rock) == 2.5f);

  // Lock and door state (new system).
  ObjectRef chest2{0x4D};
  check("not locked by default", !bindings.IsLocked(chest2));
  bindings.SetLocked(chest2, true);
  bindings.SetLockLevel(chest2, 50);
  check("locked after Lock(true)", bindings.IsLocked(chest2));
  check("lock level 50", bindings.GetLockLevel(chest2) == 50);
  bindings.SetLocked(chest2, false);
  check("unlocked after Lock(false)", !bindings.IsLocked(chest2));
  ObjectRef door{0x5E};
  check("door closed by default (3)", bindings.GetOpenState(door) == 3);
  bindings.SetOpen(door, true);
  check("door open after SetOpen (1)", bindings.GetOpenState(door) == 1);

  // Player controls (new system).
  check("movement controls enabled by default", bindings.IsPlayerControlEnabled(0));
  bindings.SetPlayerControl(0, false);
  check("movement controls disabled after toggle", !bindings.IsPlayerControlEnabled(0));
  check("fighting controls still enabled", bindings.IsPlayerControlEnabled(1));
  bindings.SetPlayerControl(0, true);
  check("movement controls re-enabled", bindings.IsPlayerControlEnabled(0));

  std::printf("%s (%d failures)\n", failures ? "BINDINGSTEST FAILED" : "BINDINGSTEST PASSED",
              failures);
  return failures ? 1 : 0;
}
