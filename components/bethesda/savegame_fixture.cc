#include "components/bethesda/savegame_fixture.h"

#include <base/memory/move.h>
#include <base/strings/xstring.h>

#include <cstring>

#include "components/bethesda/compression.h"
#include "components/bethesda/savegame_papyrus.h"

namespace rx::bethesda {
namespace {

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

// --- the file side: a table 1001 built to the measured layout ---------------

struct HeapWriter {
  base::Vector<u8> bytes;
  base::Vector<base::String> strings;

  void U8(u8 v) { bytes.push_back(v); }
  void U16(u16 v) {
    bytes.push_back(u8(v));
    bytes.push_back(u8(v >> 8));
  }
  void U32(u32 v) {
    for (int i = 0; i < 4; ++i)
      bytes.push_back(u8(v >> (8 * i)));
  }
  void U64(rx::u64 v) {
    for (int i = 0; i < 8; ++i)
      bytes.push_back(u8(v >> (8 * i)));
  }
  void F32(f32 v) {
    u32 bits;
    std::memcpy(&bits, &v, 4);
    U32(bits);
  }
  // A form id the save resolved against the first master, which is RefID kind 1
  // (three big endian bytes with the kind in the top two).
  void MasterRef(u32 form_id) {
    bytes.push_back(u8(0x40 | ((form_id >> 16) & 0x3f)));
    bytes.push_back(u8(form_id >> 8));
    bytes.push_back(u8(form_id));
  }

  u32 Str(const base::String& value) {
    for (size_t i = 0; i < strings.size(); ++i)
      if (strings[i] == value)
        return static_cast<u32>(i);
    strings.push_back(value);
    return static_cast<u32>(strings.size() - 1);
  }
};

// One script instance's worth of the file: a definition, a header row and a
// data block. Two scripts, so the alias key and the plain form key both appear.
base::Vector<u8> BuildSyntheticPapyrusHeapTable() {
  HeapWriter w;
  // The strings have to exist before the table can be written, so the indices
  // are taken first and the header is emitted around them below.
  const u32 s_empty = w.Str("");
  const u32 s_script = w.Str("DoorScript");
  const u32 s_alias_script = w.Str("HouseAliasScript");
  const u32 s_object_reference = w.Str("ObjectReference");
  const u32 s_referencealias = w.Str("ReferenceAlias");
  const u32 s_open = w.Str("::Open_var");
  const u32 s_bool = w.Str("Bool");
  const u32 s_count = w.Str("::Count_var");
  const u32 s_int = w.Str("Int");
  const u32 s_owner = w.Str("::Owner_var");
  const u32 s_name = w.Str("::Name_var");
  const u32 s_string = w.Str("String");
  const u32 s_price = w.Str("::Price_var");
  const u32 s_float = w.Str("Float");
  const u32 s_rooms = w.Str("::Rooms_var");
  const u32 s_locked = w.Str("Locked");
  const u32 s_bought = w.Str("::Bought_var");

  HeapWriter out;
  out.strings = w.strings;
  out.U16(6);
  out.U32(static_cast<u32>(out.strings.size()));
  for (const base::String& s : out.strings) {
    out.U16(static_cast<u16>(s.size()));
    for (size_t i = 0; i < s.size(); ++i)
      out.U8(static_cast<u8>(s[i]));
  }

  // Script definitions. DoorScript declares five members of its own;
  // HouseAliasScript declares one and inherits DoorScript's five, which is what
  // makes the parent-chain-first flattening observable.
  out.U32(2);
  out.U32(s_script);
  out.U32(s_empty);
  out.U32(5);
  out.U32(s_open);
  out.U32(s_bool);
  out.U32(s_count);
  out.U32(s_int);
  out.U32(s_owner);
  out.U32(s_object_reference);
  out.U32(s_name);
  out.U32(s_string);
  out.U32(s_price);
  out.U32(s_float);

  out.U32(s_alias_script);
  out.U32(s_script);  // parent
  out.U32(1);
  out.U32(s_rooms);
  out.U32(s_int);

  // Instance header rows, 20 bytes each.
  constexpr rx::u64 kDoorId = 0x0000021200001000ull;
  constexpr rx::u64 kAliasId = 0x0000021200002000ull;
  out.U32(2);
  out.U64(kDoorId);
  out.U32(s_script);
  out.U16(0);       // kind
  out.U16(0xffff);  // no alias
  out.MasterRef(0x0001a2b3);
  out.U8(1);

  out.U64(kAliasId);
  out.U32(s_alias_script);
  out.U16(0);
  out.U16(7);  // alias index
  out.MasterRef(0x000c1a1f);
  out.U8(1);

  out.U32(0);  // no heap references

  // One int array, referenced from the alias instance's inherited Count member.
  constexpr rx::u64 kArrayId = 0x0000021200003000ull;
  out.U32(1);
  out.U64(kArrayId);
  out.U8(static_cast<u8>(PapyrusValueType::kInt));
  out.U32(3);

  out.U32(0);  // runtime counter
  out.U32(0);  // no active scripts

  // Data blocks, in the same order as the header rows and repeating their ids.
  out.U64(kDoorId);
  out.U8(0x0b);
  out.U32(s_locked);  // current state
  out.U32(0);
  out.U32(5);
  out.U8(5);  // Open = true
  out.U32(1);
  out.U8(3);  // Count = -4
  out.U32(static_cast<u32>(-4));
  out.U8(1);  // Owner = the alias instance
  out.U32(s_object_reference);
  out.U64(kAliasId);
  out.U8(2);  // Name
  out.U32(s_referencealias);
  out.U8(4);  // Price = 2.5
  out.F32(2.5f);

  out.U64(kAliasId);
  out.U8(0x0b);
  out.U32(s_empty);  // default state
  out.U32(0);
  out.U32(6);
  out.U8(5);  // inherited Open = false
  out.U32(0);
  out.U8(13);  // inherited Count = the int array
  out.U64(kArrayId);
  out.U8(1);  // inherited Owner = None
  out.U32(s_object_reference);
  out.U64(0);
  out.U8(2);  // inherited Name
  out.U32(s_bought);
  out.U8(4);  // inherited Price
  out.F32(0.0f);
  out.U8(3);  // its own Rooms = 3
  out.U32(3);

  out.U64(kArrayId);
  out.U8(3);
  out.U32(11);
  out.U8(3);
  out.U32(22);
  out.U8(3);
  out.U32(33);
  return base::move(out.bytes);
}

}  // namespace

base::Vector<u8> BuildSyntheticPapyrusHeap() {
  return BuildSyntheticPapyrusHeapTable();
}

base::Span<const u32> SyntheticSkyrimFormIds() {
  return base::Span<const u32>(kSyntheticFormIds,
                               sizeof(kSyntheticFormIds) / sizeof(*kSyntheticFormIds));
}

base::Vector<u8> BuildSyntheticSkyrimLeSave() {
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
  base::Vector<u8> packed = ZlibDeflate(ByteSpan(plain, sizeof(plain)));
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

base::Vector<u8> BuildSyntheticFallout4Save() {
  base::Vector<u8> header;
  PutU32(header, 15);  // header version
  PutU32(header, 7);
  PutWString(header, "Nate");
  PutU32(header, 12);
  PutWString(header, "Sanctuary Hills");
  PutWString(header, "5d.16h.29m.5 days.16 hours.29 minutes");
  PutWString(header, "HumanRace");
  PutU16(header, 1);
  PutF32(header, 0.0f);
  PutF32(header, 200.0f);
  for (int i = 0; i < 8; ++i)
    PutU8(header, 0);  // FILETIME
  PutU32(header, kShotW);
  PutU32(header, kShotH);

  base::Vector<u8> file;
  PutBytes(file, "FO4_SAVEGAME", 12);
  PutU32(file, u32(header.size()));
  PutBytes(file, header.data(), header.size());
  // Four bytes a pixel, unlike Skyrim LE's three.
  for (u32 i = 0; i < kShotW * kShotH * 4; ++i)
    PutU8(file, u8(i));

  const size_t body_base = file.size();

  base::Vector<u8> body;
  PutU8(body, 68);                    // form version
  PutWString(body, "1.10.163.0");     // game version, Skyrim writes none
  base::Vector<u8> plugin_info;
  PutU8(plugin_info, 1);
  PutWString(plugin_info, "Fallout4.esm");
  PutU16(plugin_info, 1);  // light plugin count
  PutWString(plugin_info, "ccBGSFO4044-HellfirePowerArmor.esl");
  PutU32(body, u32(plugin_info.size()));
  PutBytes(body, plugin_info.data(), plugin_info.size());

  const size_t flt_at = body.size();
  for (int i = 0; i < 25; ++i)
    PutU32(body, 0);

  const size_t globals_at = body.size();
  base::Vector<u8> globals;
  PutU8(globals, u8(1 << 2));
  PutRefId(globals, 1, 0x38);
  PutF32(globals, 12.5f);
  PutU32(body, 3);  // record type: global variables, the same slot as Skyrim
  PutU32(body, u32(globals.size()));
  PutBytes(body, globals.data(), globals.size());

  const size_t change_forms_at = body.size();
  // FACT, which Fallout 4 writes as 30 where Skyrim writes 31.
  PutRefId(body, 1, 0x0001CBED);
  PutU32(body, 0x80000000);
  PutU8(body, u8((0u << 6) | 30u));
  PutU8(body, 68);
  PutU8(body, 4);
  PutU8(body, 0);
  PutBytes(body, "\x01\x02\x03\x04", 4);
  // REFR, which both games write as 0.
  PutRefId(body, 1, 0x14);
  PutU32(body, 0x00000002);
  PutU8(body, u8((0u << 6) | 0u));
  PutU8(body, 68);
  PutU8(body, 2);
  PutU8(body, 0);
  PutBytes(body, "\xaa\xbb", 2);

  const size_t form_ids_at = body.size();
  PutU32(body, 1);
  PutU32(body, 0x0100BEEF);

  auto patch = [&](size_t index, u32 value) {
    const size_t at = flt_at + index * 4;
    for (int i = 0; i < 4; ++i)
      body[at + size_t(i)] = u8(value >> (8 * i));
  };
  patch(0, u32(body_base + form_ids_at));
  patch(2, u32(body_base + globals_at));
  patch(4, u32(body_base + change_forms_at));
  patch(6, 1);
  patch(9, 2);

  PutBytes(file, body.data(), body.size());
  return file;
}

}  // namespace rx::bethesda
