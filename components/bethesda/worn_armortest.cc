// worn_armortest: the ARMO -> ARMA -> model walk that decides which mesh an
// equipped armour puts on a body.
//
// It authors a synthetic plugin holding one armour and four armatures, loads it
// through RecordStore and asserts which armature each lookup lands on, so the
// race preference, the male/female choice and the "the armour's own MOD2 is the
// ground model, not the worn one" rule are all checked without game data.
//
// The shapes come from real records: DLC1ArmorDawnguardBootsLight (0x0200F400)
// writes BOD2 0x00000080 (slot 37) and one MODL, 0x0200F3FF, whose MOD2 is
// DLC01\Armor\Dawnguard\DawnguardBoots1_1.nif while the armour's own MOD2 is
// the ...GND.nif ground model.

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include <cstdio>
#include <cstring>
#include <filesystem>

#include "components/bethesda/game_profile.h"
#include "components/bethesda/load_order.h"
#include "components/bethesda/worn_armor.h"
#include "components/bethesda/writer.h"
#include "core/types.h"

using namespace rx;
using namespace rx::bethesda;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    ++g_failures;
}

constexpr u32 kArmo = FourCc('A', 'R', 'M', 'O');
constexpr u32 kArma = FourCc('A', 'R', 'M', 'A');
constexpr u32 kMisc = FourCc('M', 'I', 'S', 'C');
constexpr u32 kModl = FourCc('M', 'O', 'D', 'L');
constexpr u32 kMod2 = FourCc('M', 'O', 'D', '2');
constexpr u32 kMod3 = FourCc('M', 'O', 'D', '3');
constexpr u32 kBod2 = FourCc('B', 'O', 'D', '2');
constexpr u32 kRnam = FourCc('R', 'N', 'A', 'M');

// The races the armatures below are authored against. 0x19 is DefaultRace, the
// id every race-agnostic armature in Skyrim names.
constexpr u32 kDefaultRace = 0x00000019;
constexpr u32 kWornRace = 0x00000900;
constexpr u32 kOtherRace = 0x00000901;

constexpr u32 kArmaOther = 0x00000910;    // fits kOtherRace only
constexpr u32 kArmaDefault = 0x00000911;  // fits DefaultRace
constexpr u32 kArmaListed = 0x00000912;   // lists kWornRace among its extra races
constexpr u32 kArmaNoModel = 0x00000913;  // names the race but ships no mesh

constexpr u32 kCuirass = 0x00000920;
constexpr u32 kBoots = 0x00000921;
constexpr u32 kModelless = 0x00000922;
constexpr u32 kIngot = 0x00000923;

GameProfile TestProfile() {
  GameProfile profile;
  profile.game = Game::kSkyrimSe;
  profile.name = "test";
  profile.plugin_version = 1.0f;
  return profile;
}

base::Vector<u8> Text(const char* s) {
  base::Vector<u8> out;
  for (const char* p = s; *p; ++p)
    out.push_back(static_cast<u8>(*p));
  out.push_back(0);
  return out;
}

void AddArmature(PluginWriter& writer,
                 u32 form,
                 const char* editor_id,
                 u32 race,
                 const char* male_model,
                 const char* female_model,
                 const base::Vector<u32>& extra_races) {
  RecordBuilder rec(kArma, RawFormId{form});
  rec.EditorId(editor_id);
  rec.FieldPod(kRnam, race);
  if (male_model) {
    const base::Vector<u8> m = Text(male_model);
    rec.Field(kMod2, ByteSpan(m.data(), m.size()));
  }
  if (female_model) {
    const base::Vector<u8> f = Text(female_model);
    rec.Field(kMod3, ByteSpan(f.data(), f.size()));
  }
  for (u32 extra : extra_races)
    rec.FieldPod(kModl, extra);
  writer.AddRecord(rec.record());
}

void AddArmor(PluginWriter& writer,
              u32 form,
              const char* editor_id,
              u32 slots,
              const char* ground_model,
              const base::Vector<u32>& armatures) {
  RecordBuilder rec(kArmo, RawFormId{form});
  rec.EditorId(editor_id);
  const base::Vector<u8> g = Text(ground_model);
  rec.Field(kMod2, ByteSpan(g.data(), g.size()));
  base::Vector<u8> body(8, 0);
  std::memcpy(body.data(), &slots, sizeof(slots));
  rec.Field(kBod2, ByteSpan(body.data(), body.size()));
  for (u32 arma : armatures)
    rec.FieldPod(kModl, arma);
  writer.AddRecord(rec.record());
}

void WritePlugin(const GameProfile& profile, const base::String& path) {
  PluginWriter writer(profile);
  writer.set_author("wornarmor").set_master(true);

  AddArmature(writer, kArmaOther, "OtherRaceAA", kOtherRace, "armor/other/male.nif", nullptr, {});
  AddArmature(writer, kArmaDefault, "DefaultAA", kDefaultRace, "armor/default/male.nif",
              "armor/default/female.nif", {});
  AddArmature(writer, kArmaListed, "ListedAA", kOtherRace, "armor/listed/male.nif",
              "armor/listed/female.nif", {kWornRace});
  AddArmature(writer, kArmaNoModel, "NoModelAA", kWornRace, nullptr, nullptr, {});

  // The armour's own MOD2 is the ground model; every lookup below has to walk
  // past it into an armature.
  AddArmor(writer, kCuirass, "TestCuirass", BipedSlotBit(kBipedSlotBody), "armor/test/gnd.nif",
           {kArmaNoModel, kArmaOther, kArmaDefault, kArmaListed});
  AddArmor(writer, kBoots, "TestBoots", BipedSlotBit(kBipedSlotFeet), "armor/boots/gnd.nif",
           {kArmaOther});
  AddArmor(writer, kModelless, "TestModelless", BipedSlotBit(kBipedSlotHands), "armor/none/gnd.nif",
           {kArmaNoModel});

  RecordBuilder ingot(kMisc, RawFormId{kIngot});
  ingot.EditorId("TestIngot");
  u32 value = 1;
  ingot.FieldPod(FourCc('D', 'A', 'T', 'A'), value);
  writer.AddRecord(ingot.record());

  writer.Save(path);
}

}  // namespace

int main() {
  std::puts("worn armor: ARMO -> ARMA -> model");
  const base::String dir = std::filesystem::temp_directory_path().string();
  const base::String path = dir + "/WornArmor.esm";
  const GameProfile profile = TestProfile();
  WritePlugin(profile, path);

  LoadOrder order;
  order.Append("WornArmor.esm");
  RecordStore store;
  Check("the synthetic plugin loads", store.LoadAll(dir, order, profile));

  const GlobalFormId cuirass{0, kCuirass};
  const GlobalFormId worn_race{0, kWornRace};

  WornArmor piece;
  Check("a cuirass resolves for the worn race",
        ResolveWornArmor(store, cuirass, worn_race, /*female=*/false, &piece));
  // ListedAA is authored for another race but names kWornRace among its extra
  // races, which beats the DefaultRace armature standing next to it.
  Check("it lands on the armature that lists that race, not on DefaultRace",
        piece.armature.local_id == kArmaListed && piece.model == "armor/listed/male.nif");
  Check("the ground model on the armour itself is not what came back",
        piece.model != "armor/test/gnd.nif");
  Check("the body slot travels with it",
        piece.slots == BipedSlotBit(kBipedSlotBody) && piece.slots == 0x00000004u);

  Check("a female actor takes the same armature's other model",
        ResolveWornArmor(store, cuirass, worn_race, /*female=*/true, &piece) &&
            piece.armature.local_id == kArmaListed && piece.model == "armor/listed/female.nif");

  // A race no armature names falls back to the DefaultRace one, which is what
  // makes a generic armour fit every race in the game.
  Check("an unnamed race falls back to the DefaultRace armature",
        ResolveWornArmor(store, cuirass, GlobalFormId{0, 0x00000905}, false, &piece) &&
            piece.armature.local_id == kArmaDefault && piece.model == "armor/default/male.nif");

  // Only one armature and it fits nobody: still worn, because an armour that
  // resolves to nothing is an actor walking around missing a piece.
  Check("an armour whose only armature fits another race still renders",
        ResolveWornArmor(store, GlobalFormId{0, kBoots}, worn_race, false, &piece) &&
            piece.armature.local_id == kArmaOther && piece.model == "armor/other/male.nif" &&
            piece.slots == BipedSlotBit(kBipedSlotFeet));

  Check("an armature with no model resolves nothing rather than an empty path",
        !ResolveWornArmor(store, GlobalFormId{0, kModelless}, worn_race, false, &piece));
  Check("a form that is not an armour resolves nothing",
        !ResolveWornArmor(store, GlobalFormId{0, kIngot}, worn_race, false, &piece));
  Check("a form that is not in the load order resolves nothing",
        !ResolveWornArmor(store, GlobalFormId{0, 0x00000999}, worn_race, false, &piece));

  // The slot numbering itself: Skyrim counts from 30, so the mask bit is the
  // slot minus 30.
  Check("slot 30 is bit 0 and slot 37 is bit 7",
        BipedSlotBit(kBipedSlotHead) == 0x1u && BipedSlotBit(kBipedSlotFeet) == 0x80u);

  std::error_code ec;
  std::filesystem::remove(path.c_str(), ec);

  if (g_failures == 0) {
    std::puts("worn armor: all checks passed");
    return 0;
  }
  std::printf("worn armor: %d checks FAILED\n", g_failures);
  return 1;
}
