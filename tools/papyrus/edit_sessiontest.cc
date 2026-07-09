// edit_sessiontest: end-to-end check for the override/authoring layer
// (engine/bethesda/edit_session.*). It authors a base master with the
// writer, loads it through RecordStore, then uses EditSession to override a
// vanilla record and create a new form that references base records. It saves a
// patch plugin, reloads [Base.esm, Patch.esp] merged, and verifies the override
// wins and the authored references resolve. No game data, runs in the ctest
// gate.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "bethesda/edit_session.h"
#include "bethesda/game_profile.h"
#include "bethesda/load_order.h"
#include "bethesda/plugin.h"
#include "bethesda/raw_rewriter.h"
#include "bethesda/writer.h"
#include "core/types.h"

using namespace rx;
using namespace rx::bethesda;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

constexpr u32 kWeap = FourCc('W', 'E', 'A', 'P');
constexpr u32 kMisc = FourCc('M', 'I', 'S', 'C');
constexpr u32 kFlst = FourCc('F', 'L', 'S', 'T');
constexpr u32 kData = FourCc('D', 'A', 'T', 'A');
constexpr u32 kLnam = FourCc('L', 'N', 'A', 'M');
constexpr u32 kCell = FourCc('C', 'E', 'L', 'L');
constexpr u32 kRefr = FourCc('R', 'E', 'F', 'R');
constexpr u32 kName = FourCc('N', 'A', 'M', 'E');
constexpr u32 kDial = FourCc('D', 'I', 'A', 'L');
constexpr u32 kInfo = FourCc('I', 'N', 'F', 'O');
constexpr u32 kWrld = FourCc('W', 'R', 'L', 'D');
constexpr u32 kXclc = FourCc('X', 'C', 'L', 'C');
constexpr u32 kAlch = FourCc('A', 'L', 'C', 'H');
constexpr u32 kFull = FourCc('F', 'U', 'L', 'L');
constexpr u32 kEdid = FourCc('E', 'D', 'I', 'D');

// Minimal reader for a non-length-prefixed .strings file: returns the text for
// `id`, mirroring engine/bethesda/strings.cc.
std::string ReadStringsEntry(const std::string& path, u32 id) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) return {};
  size_t size = static_cast<size_t>(file.tellg());
  file.seekg(0);
  std::vector<u8> bytes(size);
  file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
  if (size < 8) return {};
  u32 count = 0;
  std::memcpy(&count, bytes.data(), 4);
  size_t dir_end = 8 + static_cast<size_t>(count) * 8;
  for (u32 i = 0; i < count; ++i) {
    u32 eid = 0, offset = 0;
    std::memcpy(&eid, bytes.data() + 8 + i * 8, 4);
    std::memcpy(&offset, bytes.data() + 8 + i * 8 + 4, 4);
    if (eid != id) continue;
    size_t pos = dir_end + offset;
    if (pos >= size) return {};
    const char* s = reinterpret_cast<const char*>(bytes.data() + pos);
    return std::string(s, strnlen(s, size - pos));
  }
  return {};
}

// A game profile with no forced base masters, so LoadAll is happy with just the
// synthetic plugins this test writes.
GameProfile TestProfile() {
  GameProfile profile;
  profile.game = Game::kSkyrimSe;
  profile.name = "test";
  profile.plugin_version = 1.0f;
  return profile;
}

// WEAP DATA = { u32 value; f32 weight; u16 damage }.
std::vector<u8> WeaponData(u32 value, float weight, u16 damage) {
  std::vector<u8> d(10);
  std::memcpy(d.data() + 0, &value, 4);
  std::memcpy(d.data() + 4, &weight, 4);
  std::memcpy(d.data() + 8, &damage, 2);
  return d;
}

void WriteBaseMaster(const GameProfile& profile, const std::string& path) {
  PluginWriter base(profile);
  base.set_author("base").set_master(true);

  RecordBuilder sword(kWeap, RawFormId{0x00000800});
  sword.EditorId("IronSword");
  std::vector<u8> data = WeaponData(10, 8.0f, 7);
  sword.Field(kData, ByteSpan(data.data(), data.size()));
  base.AddRecord(sword.record());

  RecordBuilder ingot(kMisc, RawFormId{0x00000801});
  ingot.EditorId("IronIngot");
  u32 value = 1;
  ingot.FieldPod(kData, value);
  base.AddRecord(ingot.record());

  base.Save(path);
}

}  // namespace

int main() {
  const std::string dir = std::filesystem::temp_directory_path().string();
  const std::string base_path = dir + "/Base.esm";
  const std::string patch_path = dir + "/Patch.esp";
  const GameProfile profile = TestProfile();

  std::printf("author base master:\n");
  WriteBaseMaster(profile, base_path);

  LoadOrder order;
  order.Append("Base.esm");
  RecordStore store;
  Check("load base", store.LoadAll(dir, order, profile));

  const GlobalFormId iron_sword{0, 0x800};
  const GlobalFormId iron_ingot{0, 0x801};
  Check("base has IronSword", store.Find(iron_sword) != nullptr);
  Check("base has IronIngot", store.Find(iron_ingot) != nullptr);

  std::printf("edit session:\n");
  EditSession session(store, order, profile);

  // Override the vanilla sword's damage 7 -> 25.
  Check("override IronSword", session.Override(iron_sword));
  std::vector<u8> buffed = WeaponData(10, 8.0f, 25);
  Check("edit damage", session.SetField(iron_sword, kData, ByteSpan(buffed.data(), buffed.size())));

  // Create a new form list that references both base records.
  GlobalFormId loot = session.Create(kFlst);
  Check("created form is output-plugin", loot.plugin == kOutputPlugin);
  session.SetEditorId(loot, "MyLootList");
  Check("ref sword", session.AddReference(loot, kLnam, iron_sword));
  Check("ref ingot", session.AddReference(loot, kLnam, iron_ingot));

  // an interior cell with a placed reference (temporary child).
  GlobalFormId room = session.Create(kCell);
  session.SetEditorId(room, "TestRoom");
  GlobalFormId chest = session.Create(kRefr);
  Check("chest base ref", session.SetReference(chest, kName, iron_ingot));
  std::vector<u8> placement(24, 0);  // REFR DATA: position + rotation
  session.SetField(chest, kData, ByteSpan(placement.data(), placement.size()));
  Check("place chest in room", session.PlaceInInteriorCell(room, chest, /*persistent=*/false));

  // a dialogue topic with one info.
  GlobalFormId topic = session.Create(kDial);
  session.SetEditorId(topic, "TestTopic");
  GlobalFormId info = session.Create(kInfo);
  Check("add topic info", session.AddTopicInfo(topic, info));

  // an exterior worldspace cell at grid (5,5) with a placed reference.
  GlobalFormId world = session.Create(kWrld);
  session.SetEditorId(world, "TestWorld");
  GlobalFormId ext_cell = session.Create(kCell);
  i32 grid[2] = {5, 5};
  session.SetField(ext_cell, kXclc, ByteSpan(reinterpret_cast<const u8*>(grid), sizeof(grid)));
  GlobalFormId tree = session.Create(kRefr);
  session.SetReference(tree, kName, iron_ingot);
  session.SetField(tree, kData, ByteSpan(placement.data(), placement.size()));
  Check("place tree in exterior", session.PlaceInExteriorCell(world, ext_cell, tree));

  Check("masters is [Base.esm]",
        session.masters().size() == 1 && session.masters()[0] == "Base.esm");
  Check("save patch", session.Save(patch_path, {"Patch.esp", "test", "", false, false}));

  std::printf("reopen patch:\n");
  auto patch = PluginFile::Open(patch_path, profile);
  Check("patch opens", patch.has_value());
  Check("patch masters Base.esm",
        patch && patch->masters().size() == 1 && patch->masters()[0] == "Base.esm");
  // WEAP override + FLST + (CELL+REFR) + (DIAL+INFO) + (WRLD+CELL+REFR) = 9.
  Check("patch has 9 records", patch && patch->record_count() == 9);

  std::printf("reload merged order:\n");
  LoadOrder order2;
  order2.Append("Base.esm");
  order2.Append("Patch.esp");
  RecordStore merged;
  Check("load merged", merged.LoadAll(dir, order2, profile));

  // The override must win, with the buffed damage.
  const RecordStore::StoredRecord* sword_stored = merged.Find(iron_sword);
  Check("sword winner is patch", sword_stored && sword_stored->winning_plugin == 1);
  Record sword_rec;
  Check("parse winning sword", merged.Parse(iron_sword, &sword_rec));
  const Subrecord* sword_data = sword_rec.Find(kData);
  u16 damage = 0;
  if (sword_data && sword_data->data.size() >= 10) std::memcpy(&damage, sword_data->data.data() + 8, 2);
  Check("override damage is 25", damage == 25);

  // The created form list resolves back to the two base records.
  const GlobalFormId loot_global{1, 0x800};  // defined by Patch (load order index 1)
  const RecordStore::StoredRecord* loot_stored = merged.Find(loot_global);
  Check("created FLST present", loot_stored != nullptr && loot_stored->header.type == kFlst);
  Record loot_rec;
  std::vector<GlobalFormId> refs;
  if (merged.Parse(loot_global, &loot_rec)) {
    for (const Subrecord& sub : loot_rec.subrecords) {
      if (sub.type == kLnam && sub.data.size() >= 4) {
        u32 raw;
        std::memcpy(&raw, sub.data.data(), 4);
        refs.push_back(merged.ResolveFrom(RawFormId{raw}, loot_stored->winning_plugin));
      }
    }
  }
  Check("FLST has 2 refs", refs.size() == 2);
  Check("ref 0 -> IronSword", refs.size() > 0 && refs[0] == iron_sword);
  Check("ref 1 -> IronIngot", refs.size() > 1 && refs[1] == iron_ingot);

  // the interior cell and its placed reference come back through the
  // loader's own interior index.
  std::printf("nested content:\n");
  GlobalFormId room_global = merged.FindInteriorCell("TestRoom");
  Check("interior cell found", room_global.plugin != 0xffff);
  const auto* room_refs = merged.InteriorRefs(room_global);
  Check("cell has 1 ref", room_refs != nullptr && room_refs->size() == 1);
  const GlobalFormId chest_global{1, chest.local_id};
  Check("ref is the chest", room_refs && room_refs->size() == 1 && (*room_refs)[0] == chest_global.packed());

  // the dialogue topic's info comes back through the topic index.
  const GlobalFormId topic_global{1, topic.local_id};
  const GlobalFormId info_global{1, info.local_id};
  const auto* infos = merged.TopicInfos(topic_global);
  Check("topic has 1 info", infos != nullptr && infos->size() == 1);
  Check("info is linked", infos && infos->size() == 1 && (*infos)[0] == info_global.packed());

  // the exterior worldspace, cell and placed reference come back through
  // the loader's exterior grid.
  GlobalFormId world_global = merged.FindWorldspace("TestWorld");
  Check("worldspace found", world_global.plugin != 0xffff);
  const auto* grid_cells = merged.ExteriorCells(world_global);
  Check("exterior grid built", grid_cells != nullptr);
  const RecordStore::ExteriorCell* ext = grid_cells ? grid_cells->find(RecordStore::GridKey(5, 5)) : nullptr;
  Check("cell at (5,5)", ext != nullptr);
  const GlobalFormId tree_global{1, tree.local_id};
  Check("exterior ref is the tree",
        ext && ext->refs.size() == 1 && ext->refs[0] == tree_global.packed());

  // localized string authoring. A new potion's FULL name becomes a string
  // id backed by a strings/<plugin>_english.strings file.
  std::printf("localized strings:\n");
  const std::string loc_path = dir + "/LocMod.esp";
  EditSession lsession(store, order, profile);
  GlobalFormId potion = lsession.Create(kAlch);
  lsession.SetEditorId(potion, "TestPotion");
  Check("set localized FULL",
        lsession.SetLocalizedString(potion, kFull, "Healing Draught", StringFile::kStrings));
  Check("save localized", lsession.Save(loc_path, {"LocMod.esp", "test", "", false, false, true}));

  auto loc = PluginFile::Open(loc_path, profile);
  Check("localized plugin opens", loc.has_value());
  Check("localized flag set", loc && loc->is_localized());
  u32 full_id = 0;
  bool got_full = false;
  if (loc) {
    loc->VisitRecords([&](Record& r) {
      if (r.header.type != kAlch) return;
      const Subrecord* full = r.Find(kFull);
      if (full && full->data.size() == 4) {
        std::memcpy(&full_id, full->data.data(), 4);
        got_full = true;
      }
    });
  }
  Check("FULL is a 4-byte string id", got_full && full_id != 0);
  const std::string loc_strings = dir + "/strings/LocMod_english.strings";
  Check("string id resolves to text",
        ReadStringsEntry(loc_strings, full_id) == "Healing Draught");

  // Bridge: modify the existing Base.esm in place via EditSession + RawRewriter.
  // Buff IronSword's damage and delete IronIngot; everything else stays verbatim.
  std::printf("in-place bridge:\n");
  const u16 base_index = order.IndexOf("Base.esm");
  auto rewriter = RawRewriter::Open(base_path);
  Check("rewriter opens base", rewriter.has_value());
  EditSession inplace(store, order, profile);
  Check("set in-place target", inplace.SetInPlaceTarget(base_index));
  Check("override sword in place", inplace.Override(iron_sword));
  std::vector<u8> reforged = WeaponData(10, 8.0f, 99);
  inplace.SetField(iron_sword, kData, ByteSpan(reforged.data(), reforged.size()));
  Check("remove ingot in place", inplace.Remove(iron_ingot));
  // Insert a brand new weapon into the existing plugin (non-colliding id).
  GlobalFormId blade = inplace.Create(kWeap);
  inplace.SetEditorId(blade, "InsertedBlade");
  Check("inserted id avoids collision", blade.local_id > 0x801);
  Check("apply edits to rewriter", rewriter && inplace.ApplyEditsTo(*rewriter));

  const std::string edited_path = dir + "/Base_edited.esm";
  Check("save rewritten base", rewriter && rewriter->Save(edited_path));

  LoadOrder order3;
  order3.Append("Base_edited.esm");
  RecordStore store3;
  Check("load edited base", store3.LoadAll(dir, order3, profile));
  Record edited_sword;
  Check("edited sword present", store3.Parse(GlobalFormId{0, 0x800}, &edited_sword));
  const Subrecord* edited_data = edited_sword.Find(kData);
  u16 edited_dmg = 0;
  if (edited_data && edited_data->data.size() >= 10)
    std::memcpy(&edited_dmg, edited_data->data.data() + 8, 2);
  Check("in-place damage is 99", edited_dmg == 99);
  Check("edited sword edid intact", edited_sword.GetString(kEdid) == "IronSword");
  Check("in-place form id unchanged", store3.Find(GlobalFormId{0, 0x800}) != nullptr);
  Check("ingot removed by bridge", store3.Find(GlobalFormId{0, 0x801}) == nullptr);
  Record inserted;
  Check("inserted blade present", store3.Parse(GlobalFormId{0, blade.local_id}, &inserted));
  Check("inserted blade is a WEAP", inserted.header.type == kWeap);
  Check("inserted blade edid", inserted.GetString(kEdid) == "InsertedBlade");

  std::error_code ec;
  std::filesystem::remove(loc_path, ec);
  std::filesystem::remove(loc_strings, ec);
  std::filesystem::remove(edited_path, ec);
  std::filesystem::remove(base_path, ec);
  std::filesystem::remove(patch_path, ec);

  if (g_failures == 0) {
    std::puts("edit_session: all checks passed");
    return 0;
  }
  std::printf("edit_session: %d checks FAILED\n", g_failures);
  return 1;
}
