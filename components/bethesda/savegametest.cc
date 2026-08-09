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
base::Vector<u8> BuildSkyrimLeSave() {
  base::Vector<u8> header;
  PutU32(header, 9);  // header version, below the SE cutoff
  PutU32(header, 42);
  PutWString(header, "Testinius");
  PutU32(header, 37);
  PutWString(header, "Whiterun");
  PutWString(header, "12.34.56");
  PutWString(header, "NordRace");
  PutU16(header, 0);
  PutF32(header, 100.0f);
  PutF32(header, 200.0f);
  for (int i = 0; i < 8; ++i)
    PutU8(header, 0);  // FILETIME
  PutU32(header, kShotW);
  PutU32(header, kShotH);

  base::Vector<u8> file;
  PutBytes(file, "TESV_SAVEGAME", 13);
  PutU32(file, u32(header.size()));
  PutBytes(file, header.data(), header.size());
  for (u32 i = 0; i < kShotW * kShotH * 3; ++i)
    PutU8(file, u8(i));

  const size_t body_base = file.size();

  base::Vector<u8> body;
  PutU8(body, 74);  // form version
  base::Vector<u8> plugin_info;
  PutU8(plugin_info, 2);
  PutWString(plugin_info, "Skyrim.esm");
  PutWString(plugin_info, "Update.esm");
  PutU32(body, u32(plugin_info.size()));
  PutBytes(body, plugin_info.data(), plugin_info.size());

  // The table is patched once the blocks below are laid out.
  const size_t flt_at = body.size();
  for (int i = 0; i < 25; ++i)
    PutU32(body, 0);

  const size_t globals_at = body.size();
  base::Vector<u8> globals;
  PutU8(globals, u8(3 << 2));      // vsval count of 3
  PutRefId(globals, 1, 0x3a);      // TimeScale, owned by the first master
  PutF32(globals, 20.0f);
  PutRefId(globals, 0, 2);         // one based index into the form id array
  PutF32(globals, 1.5f);
  PutRefId(globals, 2, 0x99);      // created during play
  PutF32(globals, -3.0f);
  PutU32(body, 3);  // record type: global variables
  PutU32(body, u32(globals.size()));
  PutBytes(body, globals.data(), globals.size());

  const size_t change_forms_at = body.size();
  // Uncompressed, one byte lengths.
  PutRefId(body, 1, 0x14);
  PutU32(body, 0x0b);
  PutU8(body, u8((0u << 6) | 1u));  // ACHR
  PutU8(body, 74);
  PutU8(body, 4);
  PutU8(body, 0);
  PutBytes(body, "\xde\xad\xbe\xef", 4);
  // zlib compressed, two byte lengths.
  const u8 plain[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  base::Vector<u8> packed = rx::bethesda::ZlibDeflate(rx::ByteSpan(plain, sizeof(plain)));
  PutRefId(body, 0, 3);
  PutU32(body, 0x1c);
  PutU8(body, u8((1u << 6) | 6u));  // CELL
  PutU8(body, 74);
  PutU16(body, u16(packed.size()));
  PutU16(body, u16(sizeof(plain)));
  PutBytes(body, packed.data(), packed.size());

  const size_t form_ids_at = body.size();
  PutU32(body, 3);
  for (u32 id : kSyntheticFormIds)
    PutU32(body, id);

  auto patch = [&](size_t index, u32 value) {
    const size_t at = flt_at + index * 4;
    for (int i = 0; i < 4; ++i)
      body[at + size_t(i)] = u8(value >> (8 * i));
  };
  patch(0, u32(body_base + form_ids_at));
  patch(2, u32(body_base + globals_at));
  patch(4, u32(body_base + change_forms_at));
  patch(6, 1);  // global data table 1 record count
  patch(9, 2);  // change form count

  PutBytes(file, body.data(), body.size());
  return file;
}

void TestSynthetic() {
  std::puts("synthetic skyrim le save");
  const base::Vector<u8> file = BuildSkyrimLeSave();
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
  const base::Vector<u8> file = BuildSkyrimLeSave();
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
}

}  // namespace

int main() {
  std::puts("savegametest");
  TestSynthetic();
  TestTruncation();
  TestRealSave();
  std::printf("%s\n", g_failures == 0 ? "all checks passed" : "FAILURES");
  return g_failures == 0 ? 0 : 1;
}
