// savegametest: checks for the savegame container reader.
//
// Two halves. The first builds a byte-exact synthetic Skyrim LE save (RGB
// screenshot, uncompressed body, one zlib compressed ChangeForm) and asserts
// every field back out, then truncates that buffer at a sweep of offsets to
// prove a short file is rejected instead of read past. The second opens a real
// 100% complete Skyrim SE save and asserts values read out of its bytes; it is
// skipped when the file is not on this machine.

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "components/bethesda/compression.h"
#include "components/bethesda/savegame.h"
#include "components/bethesda/savegame_fixture.h"
#include "components/bethesda/savegame_changeform.h"
#include "core/types.h"

namespace {

using rx::f32;
using rx::u16;
using rx::u32;
using rx::u8;
using rx::bethesda::ChangeFormType;
using rx::bethesda::SaveFile;
using rx::bethesda::SaveFormat;

int g_failures = 0;
void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    ++g_failures;
}

void PutU8(base::Vector<u8>& b, u8 v) {
  b.push_back(v);
}
void PutU16(base::Vector<u8>& b, u16 v) {
  b.push_back(u8(v));
  b.push_back(u8(v >> 8));
}
void PutU32(base::Vector<u8>& b, u32 v) {
  for (int i = 0; i < 4; ++i)
    b.push_back(u8(v >> (8 * i)));
}
void PutF32(base::Vector<u8>& b, f32 v) {
  u32 bits;
  std::memcpy(&bits, &v, 4);
  PutU32(b, bits);
}
void PutBytes(base::Vector<u8>& b, const void* p, size_t n) {
  const u8* src = static_cast<const u8*>(p);
  for (size_t i = 0; i < n; ++i)
    b.push_back(src[i]);
}
void PutWString(base::Vector<u8>& b, const char* s) {
  const size_t n = std::strlen(s);
  PutU16(b, u16(n));
  PutBytes(b, s, n);
}
// Three big endian bytes with the two bit kind on top.
void PutRefId(base::Vector<u8>& b, u8 kind, u32 value) {
  b.push_back(u8((kind << 6) | ((value >> 16) & 0x3f)));
  b.push_back(u8(value >> 8));
  b.push_back(u8(value));
}

constexpr u32 kShotW = 4, kShotH = 3;
constexpr u32 kSyntheticFormIds[] = {0x02001234, 0x03005678, 0x0400abcd};

// The save the reader is checked against. Field values are arbitrary but the
// layout is the real one, so the offsets in the file location table are file
// absolute and have to survive the same arithmetic a real save does.

void TestSynthetic() {
  std::puts("synthetic skyrim le save");
  const base::Vector<u8> file = rx::bethesda::BuildSyntheticSkyrimLeSave();
  const rx::ByteSpan bytes(file.data(), file.size());

  Check("detects skyrim le", rx::bethesda::DetectSaveFormat(bytes) == SaveFormat::kSkyrimLe);

  SaveFile save;
  if (!rx::bethesda::ReadSaveFile(bytes, save)) {
    Check("parses", false);
    return;
  }
  Check("format", save.format == SaveFormat::kSkyrimLe);
  Check("save number 42", save.save_number == 42);
  Check("player name", save.player_name == "Testinius");
  Check("player level 37", save.player_level == 37);
  Check("player location", save.player_location == "Whiterun");
  Check("game time string", save.game_time == "12.34.56");
  Check("play time seconds", save.in_game_seconds == 12 * 3600.0f + 34 * 60.0f + 56.0f);
  Check("two plugins", save.plugins.size() == 2 && save.plugins[0] == "Skyrim.esm" &&
                           save.plugins[1] == "Update.esm");
  Check("no light plugins", save.light_plugins.empty());
  Check("form id map", save.form_ids.size() == 3 && save.form_ids[1] == 0x03005678);

  Check("three globals", save.globals.size() == 3);
  if (save.globals.size() == 3) {
    Check("global from first master", save.globals[0].first == 0x0000003a);
    Check("global value 20", save.globals[0].second == 20.0f);
    Check("global through form id map", save.globals[1].first == 0x03005678);
    Check("global created in play", save.globals[2].first == 0xff000099);
  }

  Check("two change forms", save.change_forms.size() == 2);
  if (save.change_forms.size() == 2) {
    const auto& achr = save.change_forms[0];
    Check("achr form id", achr.form_id == 0x00000014);
    Check("achr type", achr.type == ChangeFormType::kAchr);
    Check("achr flags", achr.flags == 0x0b);
    Check("achr version 74", achr.version == 74);
    Check("achr payload", achr.data.size() == 4 && achr.data[0] == 0xde && achr.data[3] == 0xef);

    const auto& cell = save.change_forms[1];
    Check("cell form id through map", cell.form_id == 0x0400abcd);
    Check("cell type", cell.type == ChangeFormType::kCell);
    bool inflated = cell.data.size() == 12;
    for (u32 i = 0; inflated && i < 12; ++i)
      inflated = cell.data[i] == u8(i + 1);
    Check("cell payload inflated from zlib", inflated);
  }
}

// Every prefix of a valid save is a malformed save. None may be accepted and
// none may read past its own end, which is what the sanitizer build watches.
void TestTruncation() {
  std::puts("truncation");
  const base::Vector<u8> file = rx::bethesda::BuildSyntheticSkyrimLeSave();
  bool all_rejected = true;
  bool out_untouched = true;
  for (size_t n = 0; n < file.size(); ++n) {
    SaveFile save;
    save.save_number = 0xabcdef;
    if (rx::bethesda::ReadSaveFile(rx::ByteSpan(file.data(), n), save))
      all_rejected = false;
    else if (save.save_number != 0xabcdef)
      out_untouched = false;
  }
  Check("every truncation rejected", all_rejected);
  Check("rejected parse leaves out untouched", out_untouched);

  // Corrupting single bytes must either fail or parse, never trap. The length
  // and offset fields are what matters, so sweep every byte of the file.
  size_t corrupted = 0, accepted = 0;
  for (size_t at = 0; at < file.size(); ++at) {
    for (u8 patch : {u8(0x00), u8(0xff), u8(0x7f)}) {
      base::Vector<u8> copy = file;
      copy[at] = patch;
      SaveFile save;
      ++corrupted;
      if (rx::bethesda::ReadSaveFile(rx::ByteSpan(copy.data(), copy.size()), save))
        ++accepted;
    }
  }
  // Most flipped bytes land in payload or unused fields and still parse; what
  // matters is that the ones that break the structure are caught.
  Check("single byte corruption is sometimes caught", accepted < corrupted);

  SaveFile save;
  const u8 garbage[] = {'N', 'O', 'P', 'E', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  Check("garbage magic rejected",
        !rx::bethesda::ReadSaveFile(rx::ByteSpan(garbage, sizeof(garbage)), save));
  Check("garbage magic detects unknown",
        rx::bethesda::DetectSaveFormat(rx::ByteSpan(garbage, sizeof(garbage))) ==
            SaveFormat::kUnknown);
  Check("empty input rejected", !rx::bethesda::ReadSaveFile(rx::ByteSpan(), save));
}

// A real 11 MB, 100% complete Skyrim SE save. Not in the repo; point
// RX_SAVEGAME_TEST_FILE at one to run this elsewhere.
constexpr const char kRealSavePath[] =
    "/home/vince/Documents/Projects/recreation/Skyrim Special Edition 100 Percent Complete "
    "Save-53504-1-0-1628477233/pawelos4.ess";

bool ReadWholeFile(const char* path, base::Vector<u8>* out) {
  std::FILE* f = std::fopen(path, "rb");
  if (!f)
    return false;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size <= 0) {
    std::fclose(f);
    return false;
  }
  out->resize(size_t(size));
  const bool ok = std::fread(out->data(), 1, size_t(size), f) == size_t(size);
  std::fclose(f);
  return ok;
}

void TestRealSave() {
  const char* path = std::getenv("RX_SAVEGAME_TEST_FILE");
  if (!path)
    path = kRealSavePath;

  base::Vector<u8> file;
  if (!ReadWholeFile(path, &file)) {
    std::printf("real skyrim se save: skipped, %s not present\n", path);
    return;
  }
  std::puts("real skyrim se save");
  const rx::ByteSpan bytes(file.data(), file.size());
  Check("detects skyrim se", rx::bethesda::DetectSaveFormat(bytes) == SaveFormat::kSkyrimSe);

  SaveFile save;
  if (!rx::bethesda::ReadSaveFile(bytes, save)) {
    Check("parses", false);
    return;
  }
  Check("format", save.format == SaveFormat::kSkyrimSe);
  Check("save number 1", save.save_number == 1);
  Check("player name Pawelos", save.player_name == "Pawelos");
  Check("player level 271", save.player_level == 271);
  Check("player location Skyrim", save.player_location == "Skyrim");
  Check("play time 527.25.55", save.game_time == "527.25.55");
  Check("play time seconds", save.in_game_seconds == 527 * 3600.0f + 25 * 60.0f + 55.0f);

  Check("five plugins", save.plugins.size() == 5);
  if (save.plugins.size() == 5) {
    Check("first plugin", save.plugins[0] == "Skyrim.esm");
    Check("last plugin", save.plugins[4] == "Dragonborn.esm");
  }
  Check("no light plugins", save.light_plugins.empty());
  Check("59292 form ids", save.form_ids.size() == 59292);
  Check("first form id", !save.form_ids.empty() && save.form_ids[0] == 0x010009de);

  Check("933 globals", save.globals.size() == 933);
  // GameYear, GameMonth, GameDay, GameHour, GameDaysPassed, TimeScale sit at
  // 0x35..0x3a and lead the table.
  if (save.globals.size() >= 6) {
    Check("first global is GameYear", save.globals[0].first == 0x00000035);
    Check("GameYear 208", save.globals[0].second == 208.0f);
    Check("TimeScale is 0x3a", save.globals[5].first == 0x0000003a);
    Check("TimeScale 20", save.globals[5].second == 20.0f);
  }

  Check("151123 change forms", save.change_forms.size() == 151123);

  // The player reference. Its RefID kind is 1, so 0x14 resolves straight
  // through, and it is by far the largest record in the file.
  const rx::bethesda::ChangeForm* player = nullptr;
  size_t refr = 0, achr = 0, npc = 0, with_payload = 0;
  for (const auto& form : save.change_forms) {
    if (form.form_id == 0x00000014)
      player = &form;
    if (form.type == ChangeFormType::kRefr)
      ++refr;
    if (form.type == ChangeFormType::kAchr)
      ++achr;
    if (form.type == ChangeFormType::kNpc)
      ++npc;
    if (!form.data.empty())
      ++with_payload;
  }
  Check("player change form present", player != nullptr);
  if (player) {
    Check("player is an ACHR", player->type == ChangeFormType::kAchr);
    Check("player change form version 78", player->version == 78);
    Check("player payload 130087 bytes", player->data.size() == 130087);
  }
  Check("106098 REFR change forms", refr == 106098);
  Check("20658 ACHR change forms", achr == 20658);
  Check("926 NPC_ change forms", npc == 926);
  Check("137404 change forms carry a payload", with_payload == 137404);

  // Created objects: four tables in kind order, the whole 14596 byte record
  // consumed exactly. None of these ids has a change form of its own, which is
  // why this table is the only place they are described.
  using rx::bethesda::CreatedFormKind;
  size_t by_kind[4] = {};
  for (const auto& created : save.created_forms)
    ++by_kind[static_cast<size_t>(created.kind)];
  Check("394 created forms", save.created_forms.size() == 394);
  Check("43 weapon and 233 armour enchantments", by_kind[0] == 43 && by_kind[1] == 233);
  Check("95 potions and 23 poisons", by_kind[2] == 95 && by_kind[3] == 23);
  if (save.created_forms.size() == 394) {
    const auto& first = save.created_forms[0];
    Check("first created form is weapon enchantment 0xFF00110B",
          first.form_id == 0xff00110bu && first.kind == CreatedFormKind::kWeaponEnchantment);
    Check("it has one effect, EnchInfluenceConfDownFFContactLow 0x0005B451",
          first.effects.size() == 1 && first.effects[0].effect == 0x0005b451u);
    Check("10 points for 30 seconds", first.effects.size() == 1 &&
                                          first.effects[0].magnitude == 10.0f &&
                                          first.effects[0].duration == 30);
    // The first potion, a 158 point Fortify Marksman brewed for 60 seconds.
    const auto& potion = save.created_forms[43 + 233];
    Check("first potion is 0xFF006FD2",
          potion.form_id == 0xff006fd2u && potion.kind == CreatedFormKind::kPotion);
    Check("AlchFortifyMarksman 0x0003EB1B, 158 points for 60 seconds",
          potion.effects.size() == 1 && potion.effects[0].effect == 0x0003eb1bu &&
              potion.effects[0].magnitude == 158.0f && potion.effects[0].duration == 60);
    size_t with_effects = 0;
    for (const auto& created : save.created_forms) {
      if (!created.effects.empty() && created.unknown_count != 0)
        ++with_effects;
    }
    Check("every created form carries at least one effect and a non-zero count",
          with_effects == 394);
  }

  // Misc stats: the whole Stats page, 108 rows consuming all 2484 bytes of the
  // record. The values are their own cross-check -- "Days Passed" agrees with
  // the 2616 days the header's play time comes to -- and they settle what the
  // FACT crime counters are: every Bounty row reads 0 while Murders and
  // Assaults are counted, so a faction's numbers are infamy, not gold owed.
  Check("108 misc stats", save.misc_stats.size() == 108);
  u32 by_category[8] = {};
  u32 bounty_rows = 0, bounty_total = 0;
  const rx::bethesda::MiscStat* days = nullptr;
  const rx::bethesda::MiscStat* murders = nullptr;
  const rx::bethesda::MiscStat* assaults = nullptr;
  const rx::bethesda::MiscStat* found = nullptr;
  for (const rx::bethesda::MiscStat& stat : save.misc_stats) {
    if (stat.category < 8)
      ++by_category[stat.category];
    if (stat.name == "Days Passed")
      days = &stat;
    if (stat.name == "Murders")
      murders = &stat;
    if (stat.name == "Assaults")
      assaults = &stat;
    if (stat.name == "Locations Discovered")
      found = &stat;
    // The ten hold bounties, not the two lifetime totals beside them. "The Rift
    // Bounty" and "The Reach Bounty" are holds, so the two are named out.
    if (stat.name.ends_with(" Bounty") && stat.name != "Total Lifetime Bounty" &&
        stat.name != "Largest Bounty") {
      ++bounty_rows;
      bounty_total += stat.value;
    }
  }
  Check("stats fall in 7 categories, none past 6",
        by_category[0] == 28 && by_category[1] == 13 && by_category[2] == 13 &&
            by_category[3] == 11 && by_category[4] == 15 && by_category[5] == 24 &&
            by_category[6] == 4 && by_category[7] == 0);
  Check("first row is Winterhold Bounty, crime category, zero",
        !save.misc_stats.empty() && save.misc_stats[0].name == "Winterhold Bounty" &&
            save.misc_stats[0].category == 5 && save.misc_stats[0].value == 0);
  // The strongest cross-check there is between two of these tables: the stats
  // record and the global variable record are decoded by different walks off
  // different bytes, and GameDaysPassed (0x39) reads 2616.0049 against the
  // stat's 2616. Not the header's play time, which is 527 hours at the wheel.
  Check("Days Passed 2616 agrees with the GameDaysPassed global",
        days != nullptr && days->value == 2616 && save.globals.size() > 4 &&
            save.globals[4].first == 0x00000039u &&
            u32(save.globals[4].second) == days->value);
  Check("413 locations discovered", found != nullptr && found->value == 413);
  Check("169 murders and 361 assaults", murders != nullptr && assaults != nullptr &&
                                            murders->value == 169 && assaults->value == 361);
  Check("all ten hold bounties are paid off", bounty_rows == 10 && bounty_total == 0);

  // Player location. Its position is the player ACHR's transform to the byte,
  // and both name Tamriel, so the two agree about where the player is; the cell
  // coordinates beside it do not hold the cell that position falls in
  // (floor(pos / 4096) is -6,23) and are a grid centre the game lags behind.
  const rx::bethesda::SavedPlayerLocation& loc = save.player_place;
  Check("player location decoded", loc.valid);
  Check("next created id 0xFF0087C3", loc.next_object_id == 0xff0087c3u);
  Check("coordinates and position both in Tamriel 0x3C",
        loc.coord_worldspace == 0x0000003cu && loc.parent == 0x0000003cu);
  Check("grid centre -7,24", loc.cell_x == -7 && loc.cell_y == 24);
  Check("position is the player reference's own",
        loc.position[0] == -24067.296875f && loc.position[1] == 97838.046875f &&
            loc.position[2] == -13344.8779296875f);

  // Weather. Both ids resolve to records of the type they should: 0x00000812 is
  // CLMT SkyrimClimate and 0x000C8221 is WTHR SkyrimStormSnow, read back out of
  // Skyrim.esm. The sky had settled, so there is nothing to fade from.
  const rx::bethesda::SavedWeather& sky = save.weather;
  Check("weather decoded", sky.valid);
  Check("climate SkyrimClimate 0x00000812", sky.climate == 0x00000812u);
  Check("weather SkyrimStormSnow 0x000C8221", sky.weather == 0x000c8221u);
  Check("settled, so no previous weather and a full transition",
        sky.previous == 0 && sky.transition == 1.0f);
  // What pins the float triple's position: the weather record's current time is
  // bit for bit the GameHour global (0x38), decoded out of a different record
  // by a different walk. That also settles the unit as game hours.
  Check("weather clock is the GameHour global",
        save.globals.size() > 3 && save.globals[3].first == 0x00000038u &&
            sky.current_time == save.globals[3].second);

  // Ingredient pairs. Ten pairs consuming the record exactly, and every one of
  // the 20 ids resolves to an INGR record in the masters -- none to a magic
  // effect, which is what the wiki claims the second slot is.
  Check("10 ingredient pairs", save.ingredient_pairs.size() == 10);
  if (save.ingredient_pairs.size() == 10) {
    Check("first pair CritterBeeIngredient 0x000A9195 + MothWingMonarch 0x000727E0",
          save.ingredient_pairs[0].first == 0x000a9195u &&
              save.ingredient_pairs[0].second == 0x000727e0u);
    Check("last pair MountainFlower01Purple 0x00077E1E + HangingMoss 0x00057F91",
          save.ingredient_pairs[9].first == 0x00077e1eu &&
              save.ingredient_pairs[9].second == 0x00057f91u);
  }
  // The second global data table. The favourites record (109, 146 bytes) and the
  // interface record (102, 1121 bytes) are both consumed exactly by the reader,
  // and every id in them was cross-checked against the five plugins the save
  // names: the favourites are seven SPEL and four SHOU, the three histories are
  // a hundred entries each of WEAP, SPEL and SHOU and nothing else.
  Check("11 magic favourites", save.magic_favourites.size() == 11);
  if (save.magic_favourites.size() == 11) {
    Check("the first is Incinerate 0x0010F7ED",
          save.magic_favourites[0].form_id == 0x0010f7edu);
    Check("UnrelentingForceShout 0x00013E07 is the seventh",
          save.magic_favourites[6].form_id == 0x00013e07u);
    // The only one written as an index into the form id map rather than as an
    // id of the first master, so it also checks the map is being used.
    Check("the last is Dawnguard's DLC01SummonSoulHorse 0x0200C600",
          save.magic_favourites[10].form_id == 0x0200c600u);
    u32 bound = 0;
    for (const rx::bethesda::MagicFavourite& favourite : save.magic_favourites) {
      if (favourite.hotkey >= 0)
        ++bound;
    }
    // 37 hotkey slots, every one of them empty, so what a bound key looks like
    // is not observable in this file.
    Check("none of them is bound to a number key", bound == 0);
  }
  // The tail of each history. The worn weapon is DLC1DragonboneBow, which is
  // what says the tail is the newest end and not the oldest.
  Check("last used weapon DLC1DragonboneBow 0x020176F1",
        save.last_used_weapon == 0x020176f1u);
  Check("last used spell ConjureDremoraLord 0x0010DDEC, last shout Dragonrend 0x00044250",
        save.last_used_spell == 0x0010ddecu && save.last_used_shout == 0x00044250u);
}

// A Fallout 4 container, laid out the way the real ones on this machine are
// (measured: header version 15, form version 68, an RGBA thumbnail, no whole
// body codec, and a game version wstring between the form version and the
// plugin block). The change form type byte carries Fallout 4's own numbering,
// which is Skyrim's shifted down by one from INGR up.

void TestSyntheticFallout4() {
  std::puts("synthetic fallout 4 save");
  const base::Vector<u8> file = rx::bethesda::BuildSyntheticFallout4Save();
  const rx::ByteSpan bytes(file.data(), file.size());
  Check("detects fallout 4", rx::bethesda::DetectSaveFormat(bytes) == SaveFormat::kFallout4);

  SaveFile save;
  if (!rx::bethesda::ReadSaveFile(bytes, save)) {
    Check("parses", false);
    return;
  }
  Check("format", save.format == SaveFormat::kFallout4);
  Check("save number 7", save.save_number == 7);
  Check("player name", save.player_name == "Nate");
  Check("player level 12", save.player_level == 12);
  Check("player location", save.player_location == "Sanctuary Hills");
  // Days, hours, minutes, not Skyrim's hours, minutes, seconds.
  Check("play time is 5 days 16 hours 29 minutes",
        save.in_game_seconds == 5 * 86400.0f + 16 * 3600.0f + 29 * 60.0f);
  // Only reachable if the game version wstring was consumed: the plugin block
  // size sits right behind it and reading it at the wrong offset is garbage.
  Check("one plugin and one light plugin",
        save.plugins.size() == 1 && save.plugins[0] == "Fallout4.esm" &&
            save.light_plugins.size() == 1 &&
            save.light_plugins[0] == "ccBGSFO4044-HellfirePowerArmor.esl");
  Check("form id map", save.form_ids.size() == 1 && save.form_ids[0] == 0x0100BEEF);
  Check("one global", save.globals.size() == 1 && save.globals[0].first == 0x00000038 &&
                          save.globals[0].second == 12.5f);
  Check("two change forms", save.change_forms.size() == 2);
  if (save.change_forms.size() == 2) {
    // The type byte reads 30; layer 1 turns it into the FACT every layer above
    // it knows, which is Skyrim's 31.
    Check("type byte 30 decodes as FACT", save.change_forms[0].type == ChangeFormType::kFact);
    Check("type byte 0 decodes as REFR", save.change_forms[1].type == ChangeFormType::kRefr);
  }
  Check("a fallout 4 type byte maps up, a skyrim one does not",
        rx::bethesda::ChangeFormTypeOf(SaveFormat::kFallout4, 30) == ChangeFormType::kFact &&
            rx::bethesda::ChangeFormTypeOf(SaveFormat::kSkyrimSe, 30) != ChangeFormType::kFact);
  Check("the shift starts above BOOK, which both games number 13",
        rx::bethesda::ChangeFormTypeOf(SaveFormat::kFallout4, 13) == ChangeFormType::kBook &&
            rx::bethesda::ChangeFormTypeOf(SaveFormat::kSkyrimSe, 13) == ChangeFormType::kBook);
}

// A real Fallout 4 save. Not in the repo; point RX_FO4_SAVEGAME_TEST_FILE at
// one to run this elsewhere.
constexpr const char kRealFallout4SavePath[] =
    "/speed/SteamLibrary/steamapps/compatdata/377160/pfx/drive_c/users/steamuser/Documents/"
    "My Games/Fallout4/Saves/"
    "Save1_ACCC01CEM636C61697265_Vault111Cryo_000016_20190713231124_1_2.fos";

void TestRealFallout4Save() {
  const char* path = std::getenv("RX_FO4_SAVEGAME_TEST_FILE");
  if (!path)
    path = kRealFallout4SavePath;

  base::Vector<u8> file;
  if (!ReadWholeFile(path, &file)) {
    std::printf("real fallout 4 save: skipped, %s not present\n", path);
    return;
  }
  std::puts("real fallout 4 save");
  const rx::ByteSpan bytes(file.data(), file.size());
  Check("detects fallout 4", rx::bethesda::DetectSaveFormat(bytes) == SaveFormat::kFallout4);

  SaveFile save;
  if (!rx::bethesda::ReadSaveFile(bytes, save)) {
    Check("parses", false);
    return;
  }
  Check("save number 1", save.save_number == 1);
  Check("player name claire", save.player_name == "claire");
  Check("player level 1", save.player_level == 1);
  Check("player location Vault 111", save.player_location == "Vault 111");
  // The save is German, so the unit letters are T/S/M rather than d/h/m and
  // only the leading numbers are readable: 0 days, 0 hours, 16 minutes.
  Check("16 minutes played", save.in_game_seconds == 16 * 60.0f);
  Check("two plugins", save.plugins.size() == 2 && save.plugins[0] == "Fallout4.esm" &&
                           save.plugins[1] == "CBBE.esp");
  Check("no light plugins", save.light_plugins.empty());
  // With one master and one esp almost every reference is owned by the first
  // master, which RefID kind 1 spells directly, so the map stays empty.
  Check("an empty form id map", save.form_ids.empty());
  Check("910 globals", save.globals.size() == 910);
  // The created-objects record is there and holds four empty tables: Fallout 4
  // has no player enchanting or alchemy, so nothing ever lands in them.
  Check("no created forms", save.created_forms.empty());
  Check("5022 change forms", save.change_forms.size() == 5022);

  u32 by_type[64] = {};
  u32 created = 0, versions_68 = 0;
  for (const rx::bethesda::ChangeForm& form : save.change_forms) {
    by_type[static_cast<u32>(form.type) & 0x3f] += 1;
    if ((form.form_id >> 24) == 0xff)
      ++created;
    if (form.version == 68)
      ++versions_68;
  }
  Check("every change form is version 68", versions_68 == 5022);
  Check("64 references the save created", created == 64);
  Check("2838 REFR, 1411 ACHR, 62 CELL",
        by_type[u32(ChangeFormType::kRefr)] == 2838 &&
            by_type[u32(ChangeFormType::kAchr)] == 1411 &&
            by_type[u32(ChangeFormType::kCell)] == 62);
  Check("305 INFO, 281 QUST, 74 NPC_",
        by_type[u32(ChangeFormType::kInfo)] == 305 && by_type[u32(ChangeFormType::kQust)] == 281 &&
            by_type[u32(ChangeFormType::kNpc)] == 74);
  // The shifted half. Each of these was confirmed by resolving the change form
  // ids in Fallout4.esm and reading the record signature back, so the type
  // bytes 28/30/35/36/39/44 really are these records.
  Check("2 ECZN, 4 FACT, 1 SMQN, 39 SCEN, 4 PHZD, 1 LVLI",
        by_type[u32(ChangeFormType::kEczn)] == 2 && by_type[u32(ChangeFormType::kFact)] == 4 &&
            by_type[u32(ChangeFormType::kSmqn)] == 1 &&
            by_type[u32(ChangeFormType::kScen)] == 39 &&
            by_type[u32(ChangeFormType::kPhzd)] == 4 &&
            by_type[u32(ChangeFormType::kLvli)] == 1);
  // Layer 2 was derived against Skyrim change form version 78 and refuses
  // anything outside 74..78, so nothing in a Fallout 4 save decodes yet.
  u32 decoded = 0;
  for (const rx::bethesda::ChangeForm& form : save.change_forms) {
    rx::bethesda::ReferenceChange ref;
    rx::bethesda::CellChange cell;
    if (rx::bethesda::DecodeReference(form, ref) || rx::bethesda::DecodeCell(form, cell))
      ++decoded;
  }
  Check("no payload decodes: the change form version is outside the validated range",
        decoded == 0);

  // Global data numbering does NOT shift between the games the way the change
  // form types do. Fallout 4 writes the same record numbers Skyrim does, with
  // its own set: measured across all 54 Fallout 4 saves on this machine, every
  // one holds 0..11 and 100..103, 105..106, 109..111 and 113..117, and every
  // group walks to exactly the offset the file location table gives for
  // whatever follows it.
  Check("99 misc stats", save.misc_stats.size() == 99);
  u32 fo4_categories[8] = {};
  const rx::bethesda::MiscStat* caps = nullptr;
  for (const rx::bethesda::MiscStat& stat : save.misc_stats) {
    if (stat.category < 8)
      ++fo4_categories[stat.category];
    if (stat.name == "Caps Found")
      caps = &stat;
  }
  // Fallout 4 skips category 6 and uses 7, where Skyrim runs 0..6: the number
  // is a tab index in that game's own menu and means nothing across the two.
  Check("categories 0-5 and 7, none in 6",
        fo4_categories[6] == 0 && fo4_categories[7] == 17 && fo4_categories[0] == 34);
  // A save 16 minutes old, so the whole page reads zero. It being present at
  // all is the point: the table is written from the first autosave.
  Check("Caps Found present and zero on a fresh save", caps != nullptr && caps->value == 0);

  // Player location is 30 bytes here against Skyrim SE's 31: the trailing byte
  // is the later format's. The two worldspace slots differ because the player
  // is indoors -- 0x000A7FF4 is the exterior they came from and 0x000016D8 the
  // Vault 111 cryo cell the save's own header names.
  const rx::bethesda::SavedPlayerLocation& loc = save.player_place;
  Check("player location decoded", loc.valid);
  Check("outside worldspace 0x000A7FF4, standing in cell 0x000016D8",
        loc.coord_worldspace == 0x000a7ff4u && loc.parent == 0x000016d8u);
  Check("grid centre 0,-1", loc.cell_x == 0 && loc.cell_y == -1);

  Check("weather decoded", save.weather.valid && save.weather.weather != 0);
  // No alchemy in Fallout 4, and no record 112 in any of the 54 saves.
  Check("no ingredient pairs", save.ingredient_pairs.empty());
  // The second global data table is refused for the same reason. Fallout 4 does
  // number these records the same (its save carries a 102 and a 109 too) but
  // lays them out differently: 65 bytes beginning 0x0001FFFFFFFF where Skyrim
  // counts help messages, and three zero bytes where Skyrim counts favourites.
  Check("no magic favourites and no last-used forms are read out of it",
        save.magic_favourites.empty() && save.last_used_weapon == 0 &&
            save.last_used_spell == 0 && save.last_used_shout == 0);
}

}  // namespace

int main() {
  std::puts("savegametest");
  TestSynthetic();
  TestSyntheticFallout4();
  TestTruncation();
  TestRealSave();
  TestRealFallout4Save();
  std::printf("%s\n", g_failures == 0 ? "all checks passed" : "FAILURES");
  return g_failures == 0 ? 0 : 1;
}
