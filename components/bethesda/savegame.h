#ifndef COMPONENTS_BETHESDA_SAVEGAME_H
#define COMPONENTS_BETHESDA_SAVEGAME_H

// Reading a Bethesda savegame and turning it back into live world state.
//
// The layering here is deliberate, because the three jobs fail differently and
// want testing separately:
//
//   1. SaveFile   parses the CONTAINER: header, plugin list, file location
//                 table, globals, the ChangeForm block, the form-id map. Knows
//                 nothing about what a ChangeForm means.
//   2. ChangeForm decoding turns one record's versioned, type-specific,
//                 optionally-compressed payload into a typed struct.
//   3. Applying   writes those structs onto the live world (ECS, quests,
//                 inventories, the player).
//
// GAME-AGNOSTIC BY CONSTRUCTION. Skyrim LE/SE and Fallout 4/76 share this
// shape and differ in header magic, compression, and which ChangeForm types
// carry which fields. `Game` selects those; nothing above layer 1 should branch
// on the magic string.

#include <base/containers/pair.h>
#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include "core/types.h"

namespace rx::bethesda {

enum class SaveFormat {
  kUnknown,
  kSkyrimLe,   // "TESV_SAVEGAME", zlib
  kSkyrimSe,   // "TESV_SAVEGAME", header version 12+, LZ4 frame
  kFallout4,   // "FO4_SAVEGAME"
};

// A ChangeForm's record type. The values are Skyrim's on-disk enumeration, so
// do not renumber: each one was confirmed by resolving every change form in a
// Skyrim SE save through the form-id map and reading back the record's real
// signature from the masters, so they are measured, not inferred from the
// record type order. Fallout 4 numbers the same types differently and layer 1
// translates (see ChangeFormTypeOf), which is what keeps everything above it
// off the game.
enum class ChangeFormType : u8 {
  kRefr = 0, kAchr = 1, kPgre = 3, kCell = 6, kInfo = 7, kQust = 8,
  kNpc = 9, kBook = 13, kIngr = 16, kEczn = 29, kFact = 31, kWoop = 34,
  kSmqn = 36, kScen = 37, kLctn = 38, kRela = 39, kPhzd = 40, kFlst = 43,
  kLvln = 44, kLvli = 45, kEnch = 48, kInnr = 50, kUnknown = 63,
};

// The record type behind a change form's raw type byte (its low 6 bits).
//
// Fallout 4 has no INGR, so every type from there up sits one below Skyrim's:
// measured over 226623 change forms of three Fallout 4 saves by resolving each
// one's id in Fallout4.esm and reading the signature back, which puts ECZN at
// 28 (Skyrim 29), FACT at 30 (31), SMQN 35 (36), SCEN 36 (37), LCTN 37 (38),
// PHZD 39 (40), FLST 42 (43), LVLI 44 (45) and INNR, which Skyrim does not
// have, at 49. Where exactly the shift starts is not observable: no Fallout 4
// save in hand carries a change form whose type falls in 17..27.
ChangeFormType ChangeFormTypeOf(SaveFormat format, u8 type_byte);

// What a form the player made at runtime was made as. The four tables are
// stored in this order and nothing else in the save says which kind an id is.
enum class CreatedFormKind : u8 {
  kWeaponEnchantment,
  kArmorEnchantment,
  kPotion,
  kPoison,
};

struct CreatedEffect {
  u32 effect = 0;        // MGEF form id, already resolved through the save
  f32 magnitude = 0.0f;
  u32 duration = 0;      // seconds
  // The price the game computed, which is what the item is worth. Only the
  // first effect of a multi-effect item carries it; the rest read 0.
  f32 value = 0.0f;
};

// A base form the save invented: a player enchantment or an alchemy product.
// Its id is 0xFFxxxxxx and no plugin has a record for it, so this table is the
// only description of it that exists.
struct CreatedForm {
  u32 form_id = 0;
  CreatedFormKind kind = CreatedFormKind::kWeaponEnchantment;
  // Never zero and small (1..24 across the reference save), which reads like a
  // copy count; ReSaver calls it timesUsed. Nothing in the file settles it.
  u32 unknown_count = 0;
  base::Vector<CreatedEffect> effects;
};

// One record as it sits in the file: still opaque, already decompressed.
struct ChangeForm {
  u32 form_id = 0;          // resolved through the form-id map, not raw
  ChangeFormType type = ChangeFormType::kUnknown;
  u32 flags = 0;            // which optional field groups the payload carries
  u16 version = 0;
  base::Vector<u8> data;    // decompressed payload
};

// The container. Parsing this must never require a loaded game.
struct SaveFile {
  SaveFormat format = SaveFormat::kUnknown;
  u32 save_number = 0;
  base::String player_name;
  u32 player_level = 0;
  base::String player_location;   // cell/worldspace display name at save time
  base::String game_time;
  f32 in_game_seconds = 0.0f;

  // The save's own load order. A form id in the file is only meaningful
  // against this list, so applying a save to a different order must remap or
  // refuse rather than silently land on the wrong records.
  base::Vector<base::String> plugins;
  // Light (ESL) plugins, kept apart because they do not occupy a load order
  // slot: their forms live in the 0xFE000000 range, not at index plugins.size().
  base::Vector<base::String> light_plugins;

  // The form-id map every RefID in the file indexes into, kept because a
  // ChangeForm payload embeds RefIDs too and layer 2 cannot resolve them alone.
  base::Vector<u32> form_ids;

  base::Vector<ChangeForm> change_forms;
  // Global variables (form id -> value), the cheapest thing a save carries and
  // the one most gameplay systems read.
  base::Vector<base::Pair<u32, f32>> globals;
  // Forms the save itself created. Not one of them has a ChangeForm of its own,
  // so this list is their whole definition and a 0xFFxxxxxx id naming one
  // cannot be resolved out of the change form block at all.
  base::Vector<CreatedForm> created_forms;
};

// Layer 1. Returns false and leaves `out` untouched on a malformed file.
bool ReadSaveFile(ByteSpan bytes, SaveFile& out);

// Sniffs the format from the magic and header version without a full parse.
SaveFormat DetectSaveFormat(ByteSpan bytes);

}  // namespace rx::bethesda

#endif  // COMPONENTS_BETHESDA_SAVEGAME_H
