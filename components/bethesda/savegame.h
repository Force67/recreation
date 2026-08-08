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

// A ChangeForm's record type, as stored in the low 6 bits of its type byte.
// Values follow the on-disk enumeration, so do not renumber. Each one below was
// confirmed by resolving every change form in a Skyrim SE save through the
// form-id map and reading back the record's real signature from the masters, so
// the values are measured, not inferred from the record type order.
enum class ChangeFormType : u8 {
  kRefr = 0, kAchr = 1, kPgre = 3, kCell = 6, kInfo = 7, kQust = 8,
  kNpc = 9, kBook = 13, kIngr = 16, kEczn = 29, kFact = 31, kWoop = 34,
  kSmqn = 36, kScen = 37, kLctn = 38, kRela = 39, kPhzd = 40, kFlst = 43,
  kLvln = 44, kLvli = 45, kEnch = 48, kUnknown = 63,
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
};

// Layer 1. Returns false and leaves `out` untouched on a malformed file.
bool ReadSaveFile(ByteSpan bytes, SaveFile& out);

// Sniffs the format from the magic and header version without a full parse.
SaveFormat DetectSaveFormat(ByteSpan bytes);

}  // namespace rx::bethesda

#endif  // COMPONENTS_BETHESDA_SAVEGAME_H
