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
  check("damage 25 -> 55", bindings.GetActorValue(actor, "Health") == 55.0f);
  check("not dead at 55", !bindings.IsDead(actor));
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

  std::printf("%s (%d failures)\n", failures ? "BINDINGSTEST FAILED" : "BINDINGSTEST PASSED",
              failures);
  return failures ? 1 : 0;
}
